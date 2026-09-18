// tests/integration/admin/WritePathCacheInvalidationTest.cc
//
// PR #64 review (findings 1/2/4/5): write-path cache invalidation regression.
//
// The user read cache (UserReadCache) keys its entries by the READER's
// subject string — numeric id for github-flow tokens, public_sub for
// password-flow tokens — so every user/role write path must invalidate BOTH
// forms (the UserCacheInvalidator contract). The client read cache embeds
// allowedScopes/secretHash in the cached row, so every client write path
// (including resetClientSecret and updateClientScopes) must invalidate the
// client row as well.
//
// These tests call the admin services in-process with a RECORDING hook on
// each invalidator registry and assert on the recorded subjects — no cache
// entries in Redis are needed; the hook IS the assertion surface. (The
// plugin only registers its real DEL hook when the cache is enabled; the
// test config runs cache-disabled, and UserReadCacheTest re-registers its
// own hook, so replacing the single hook slot here is safe.)
//
// Storage: Postgres-only. Throwaway rows use unique suffixes and are
// hard-deleted on exit. Raw SQL here is tests/ code (the db-operations
// Mapper/Criteria rules scope to libs/ and apps/server); INSERT..RETURNING
// follows the GitHubController precedent.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include <fulla/drogon/admin/ClientManagementService.h>
#include <fulla/drogon/admin/UserAdminService.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ClientCacheInvalidator was promoted to the public include tree in v1.4.0
// (the open-platform governance write paths share the invariant);
// UserReadCache remains src-internal (relative include, UserReadCacheTest
// precedent).
#include <fulla/drogon/utils/ClientCacheInvalidator.h>
#include "../../libs/drogon/src/UserReadCache.h"

namespace
{

using UserAdmin = ::fulla::drogon::admin::UserAdminService;
using ClientAdmin = ::fulla::drogon::admin::ClientManagementService;

// Thread-safe recording sink for invalidator hooks (they fire on IO threads).
struct RecordingSink
{
    std::mutex mu;
    std::vector<std::string> entries;

    void add(const std::string &s)
    {
        std::lock_guard<std::mutex> lock(mu);
        entries.push_back(s);
    }
    size_t count(const std::string &s)
    {
        std::lock_guard<std::mutex> lock(mu);
        return static_cast<size_t>(std::count(entries.begin(), entries.end(), s));
    }
    bool empty()
    {
        std::lock_guard<std::mutex> lock(mu);
        return entries.empty();
    }
    void reset()
    {
        std::lock_guard<std::mutex> lock(mu);
        entries.clear();
    }
};

::drogon::orm::DbClientPtr dbOrNull()
{
    try
    {
        return ::drogon::app().getDbClient();
    }
    catch (...)
    {
        return nullptr;
    }
}

::drogon::HttpRequestPtr jsonRequest(
  ::drogon::HttpMethod method,
  const std::string &path,
  const std::string &body = ""
)
{
    auto req = ::drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(path);
    if (!body.empty())
    {
        req->setBody(body);
        req->addHeader("content-type", "application/json");
    }
    return req;
}

// Invoke a (req, ResponseCallback, args...) service call and wait for its
// response (nullptr = timeout). The callback is shared as the services expect.
template <typename Invoke>
::drogon::HttpResponsePtr waitResponse(Invoke &&invoke)
{
    std::promise<::drogon::HttpResponsePtr> p;
    auto f = p.get_future();
    auto cb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      [&p](const ::drogon::HttpResponsePtr &resp) { p.set_value(resp); });
    invoke(std::move(cb));
    if (f.wait_for(std::chrono::seconds(10)) != std::future_status::timeout)
        return f.get();
    return nullptr;
}

// Fire-and-forget SQL for setup/cleanup (waits for completion, ignores result).
bool runSql(const ::drogon::orm::DbClientPtr &db, const std::string &sql, const std::string &arg = "")
{
    std::promise<bool> p;
    auto f = p.get_future();
    db->execSqlAsync(
      sql,
      [&p](const ::drogon::orm::Result &) { p.set_value(true); },
      [&p](const ::drogon::orm::DrogonDbException &) { p.set_value(false); },
      arg
    );
    return f.wait_for(std::chrono::seconds(10)) != std::future_status::timeout && f.get();
}

std::string uniqueSuffix()
{
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::to_string(now % 1000000);
}

struct ThrowawayUser
{
    int32_t id = -1;
    std::string publicSub;
};

// INSERT..RETURNING to capture the DB-generated id and public_sub.
bool insertThrowawayUser(const ::drogon::orm::DbClientPtr &db, ThrowawayUser &out, const std::string &suffix)
{
    std::promise<ThrowawayUser> p;
    auto f = p.get_future();
    db->execSqlAsync(
      "INSERT INTO users (username, password_hash, salt, email, email_verified) "
      "VALUES ($1, 'x', '', $2, true) RETURNING id, public_sub",
      [&p](const ::drogon::orm::Result &r) {
          ThrowawayUser u;
          if (!r.empty())
          {
              u.id = r[0]["id"].as<int32_t>();
              u.publicSub = r[0]["public_sub"].as<std::string>();
          }
          p.set_value(u);
      },
      [&p](const ::drogon::orm::DrogonDbException &) { p.set_value(ThrowawayUser{}); },
      "cacheinv_" + suffix,
      "cacheinv_" + suffix + "@example.test"
    );
    if (f.wait_for(std::chrono::seconds(10)) == std::future_status::timeout)
        return false;
    out = f.get();
    return out.id > 0 && !out.publicSub.empty();
}

// Poll until the sink records the subject (commit-deferred invalidation).
bool waitForRecord(std::shared_ptr<RecordingSink> sink, const std::string &needle, int timeoutMs = 2000)
{
    for (int waited = 0; waited < timeoutMs; waited += 25)
    {
        if (sink->count(needle) > 0)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return sink->count(needle) > 0;
}

}  // namespace

// ===========================================================================
// User write paths must invalidate BOTH subject forms: the numeric id AND
// public_sub. A numeric-only DEL is a silent no-op against every
// password-flow-token-keyed cache entry (review finding 2).
// ===========================================================================
DROGON_TEST(Integration_P0_Admin_WritePathCacheInvalidation_UserDualForm)
{
    auto db = dbOrNull();
    if (!db)
    {
        LOG_INFO << "[skip] DB unavailable — write-path invalidation test skipped";
        return;
    }

    auto sink = std::make_shared<RecordingSink>();
    ::fulla::drogon::UserCacheInvalidator::instance().registerHook(
      [sink](const std::string &subject) { sink->add(subject); });

    ThrowawayUser user;
    if (!insertThrowawayUser(db, user, uniqueSuffix()))
    {
        LOG_ERROR << "[setup] throwaway user insert failed — aborting test";
        CHECK(true);
        return;
    }
    const std::string idStr = std::to_string(user.id);

    // --- updateUser: dual-form invalidation, synchronous before the response.
    sink->reset();
    auto resp = waitResponse([&](UserAdmin::ResponseCallback cb) {
        UserAdmin::updateUser(
          jsonRequest(::drogon::Put, "/api/admin/users/" + idStr, R"({"email_verified":true})"),
          std::move(cb),
          idStr);
    });
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == ::drogon::k200OK);
    CHECK(sink->count(idStr) >= 1);
    CHECK(sink->count(user.publicSub) >= 1);

    // --- disableUser / enableUser: same dual-form contract.
    sink->reset();
    resp = waitResponse([&](UserAdmin::ResponseCallback cb) {
        UserAdmin::disableUser(
          jsonRequest(::drogon::Put, "/api/admin/users/" + idStr + "/disable"), std::move(cb), idStr);
    });
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == ::drogon::k200OK);
    CHECK(sink->count(idStr) >= 1);
    CHECK(sink->count(user.publicSub) >= 1);

    sink->reset();
    resp = waitResponse([&](UserAdmin::ResponseCallback cb) {
        UserAdmin::enableUser(
          jsonRequest(::drogon::Post, "/api/admin/users/" + idStr + "/enable"), std::move(cb), idStr);
    });
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == ::drogon::k200OK);
    CHECK(sink->count(idStr) >= 1);
    CHECK(sink->count(user.publicSub) >= 1);

    // --- assignUserRoles: invalidation at the completion point (after the
    // inserts), dual-form.
    sink->reset();
    resp = waitResponse([&](UserAdmin::ResponseCallback cb) {
        UserAdmin::assignUserRoles(
          jsonRequest(::drogon::Put, "/api/admin/users/" + idStr + "/roles", R"({"roles":["user"]})"),
          std::move(cb),
          idStr);
    });
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == ::drogon::k200OK);
    CHECK(sink->count(idStr) >= 1);
    CHECK(sink->count(user.publicSub) >= 1);

    // --- assignUserRoles on a missing user: 404, and NO invalidation fires
    // (no write happened).
    sink->reset();
    resp = waitResponse([&](UserAdmin::ResponseCallback cb) {
        UserAdmin::assignUserRoles(
          jsonRequest(::drogon::Put, "/api/admin/users/999999999/roles", R"({"roles":["user"]})"),
          std::move(cb),
          "999999999");
    });
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == ::drogon::k404NotFound);
    CHECK(sink->empty());

    // Cleanup (roles first — FK), hard delete.
    runSql(db, "DELETE FROM user_roles WHERE user_id = $1::int4", idStr);
    runSql(db, "DELETE FROM users WHERE id = $1::int4", idStr);
}

// ===========================================================================
// Client write paths: resetClientSecret invalidates synchronously before the
// response (a rotated secret must not stay trusted); updateClientScopes
// defers invalidation to the transaction COMMIT callback (review findings 1/5).
// ===========================================================================
DROGON_TEST(Integration_P0_Admin_WritePathCacheInvalidation_ClientCommitOrdered)
{
    auto db = dbOrNull();
    if (!db)
    {
        LOG_INFO << "[skip] DB unavailable — write-path invalidation test skipped";
        return;
    }

    auto sink = std::make_shared<RecordingSink>();
    ::fulla::drogon::ClientCacheInvalidator::instance().registerHook(
      [sink](const std::string &clientId) { sink->add(clientId); });

    const std::string clientId = "cacheinv_cli_" + uniqueSuffix();
    if (!runSql(
          db,
          "INSERT INTO oauth2_clients (client_id, client_type, client_secret, salt) "
          "VALUES ($1, 'CONFIDENTIAL', 'x', 'y')",
          clientId))
    {
        LOG_ERROR << "[setup] throwaway client insert failed — aborting test";
        CHECK(true);
        return;
    }

    // --- resetClientSecret: the OLD cached row (secretHash + salt) must be
    // dropped BEFORE the caller is told the rotation succeeded.
    sink->reset();
    auto resp = waitResponse([&](ClientAdmin::ResponseCallback cb) {
        ClientAdmin::resetClientSecret(
          jsonRequest(::drogon::Post, "/api/admin/clients/" + clientId + "/reset-secret"),
          std::move(cb),
          clientId);
    });
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == ::drogon::k200OK);
    CHECK(sink->count(clientId) >= 1);  // synchronous — no poll needed

    // --- updateClientScopes: invalidation is deferred to the commit callback;
    // it must STILL fire (a lost DEL pins the old allowedScopes for 300s).
    sink->reset();
    resp = waitResponse([&](ClientAdmin::ResponseCallback cb) {
        ClientAdmin::updateClientScopes(
          jsonRequest(
            ::drogon::Put, "/api/admin/clients/" + clientId + "/scopes", R"({"scopes":["openid"]})"),
          std::move(cb),
          clientId);
    });
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == ::drogon::k200OK);
    CHECK(waitForRecord(sink, clientId));

    // Cleanup (scopes first — FK).
    runSql(db, "DELETE FROM oauth2_client_scopes WHERE client_id = $1", clientId);
    runSql(db, "DELETE FROM oauth2_clients WHERE client_id = $1", clientId);
}
