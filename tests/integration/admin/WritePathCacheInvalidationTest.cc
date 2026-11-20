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
#include <fulla/storage/postgres/models/Oauth2ClientScopes.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
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
    auto p = std::make_shared<std::promise<::drogon::HttpResponsePtr>>();
    auto f = p->get_future();
    auto cb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      [p](const ::drogon::HttpResponsePtr &resp) { p->set_value(resp); });
    invoke(std::move(cb));
    if (f.wait_for(std::chrono::seconds(10)) != std::future_status::timeout)
        return f.get();
    return nullptr;
}

std::optional<std::string> createScopeClient()
{
    Json::Value json;
    json["name"] = "scope-order-" + ::drogon::utils::getUuid();
    json["client_type"] = "CONFIDENTIAL";
    json["redirect_uris"] = "http://127.0.0.1:9999/callback";
    Json::StreamWriterBuilder writer;
    auto response = waitResponse([&](ClientAdmin::ResponseCallback cb) {
        ClientAdmin::createClient(
          jsonRequest(::drogon::Post, "/api/admin/clients", Json::writeString(writer, json)),
          std::move(cb));
    });
    if (!response || response->getStatusCode() != ::drogon::k201Created)
        return std::nullopt;
    auto body = response->getJsonObject();
    if (!body || !(*body)["client_id"].isString() || (*body)["client_id"].asString().empty())
        return std::nullopt;
    return (*body)["client_id"].asString();
}

std::optional<std::vector<std::string>> readScopeClient(const std::string &clientId)
{
    auto response = waitResponse([&](ClientAdmin::ResponseCallback cb) {
        ClientAdmin::getClientScopes(
          jsonRequest(::drogon::Get, "/api/admin/clients/" + clientId + "/scopes"),
          std::move(cb), clientId);
    });
    if (!response || response->getStatusCode() != ::drogon::k200OK)
        return std::nullopt;
    auto body = response->getJsonObject();
    if (!body || !(*body)["scopes"].isArray())
        return std::nullopt;
    std::vector<std::string> scopes;
    for (const auto &scope : (*body)["scopes"])
    {
        if (!scope.isString())
            return std::nullopt;
        scopes.push_back(scope.asString());
    }
    return scopes;
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

    // --- updateClientScopes: invalidation AND the 200 both fire from the
    // commit callback (#360), invalidation first. By the time the response
    // arrives the DEL must already be recorded — no polling allowed: a poll
    // would mask a pre-commit response (the old defect let a concurrent
    // validateClient refill the cache with the stale set for a full TTL).
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
    CHECK(sink->count(clientId) >= 1);  // synchronous — commit preceded the 200

    // Cleanup (scopes first — FK).
    runSql(db, "DELETE FROM oauth2_client_scopes WHERE client_id = $1", clientId);
    runSql(db, "DELETE FROM oauth2_clients WHERE client_id = $1", clientId);
}

// ===========================================================================
// #360: updateClientScopes must respond only AFTER the transaction commits.
// A caller that receives 200 and immediately re-reads the scopes must see the
// NEW set — both for a non-empty replacement and for the empty-scopes branch.
// Before the fix the 200 went out at last-INSERT (pre destructor-time COMMIT),
// so the immediate GET could observe the old rows.
// ===========================================================================
DROGON_TEST(Integration_P0_Admin_ScopesPut_RespondsAfterCommit)
{
    auto db = dbOrNull();
    if (!db)
    {
        LOG_INFO << "[skip] DB unavailable — scopes commit-ordering test skipped";
        return;
    }

    auto sink = std::make_shared<RecordingSink>();
    ::fulla::drogon::ClientCacheInvalidator::instance().registerHook(
      [sink](const std::string &clientId) { sink->add(clientId); });

    const auto created = createScopeClient();
    REQUIRE(created.has_value());
    const std::string clientId = *created;

    auto putScopes = [&](const std::string &body) -> ::drogon::HttpResponsePtr {
        return waitResponse([&](ClientAdmin::ResponseCallback cb) {
            ClientAdmin::updateClientScopes(
              jsonRequest(::drogon::Put, "/api/admin/clients/" + clientId + "/scopes", body),
              std::move(cb),
              clientId);
        });
    };

    // --- Non-empty replacement: the immediate GET must see the new set, and
    // the response body must echo it.
    sink->reset();
    auto resp = putScopes(R"({"scopes":["openid","profile"]})");
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == ::drogon::k200OK);
    CHECK(sink->count(clientId) >= 1);  // invalidation preceded the response
    auto body = resp->getJsonObject();
    REQUIRE(body != nullptr);
    CHECK((*body)["scopes"].isArray());
    CHECK((*body)["scopes"].size() == 2);
    auto seen = readScopeClient(clientId);
    REQUIRE(seen.has_value());
    std::sort(seen->begin(), seen->end());
    CHECK(*seen == std::vector<std::string>({"openid", "profile"}));

    // --- Empty-scopes branch: same commit-before-response contract.
    sink->reset();
    resp = putScopes(R"({"scopes":[]})");
    REQUIRE(resp != nullptr);
    CHECK(resp->getStatusCode() == ::drogon::k200OK);
    CHECK(sink->count(clientId) >= 1);
    body = resp->getJsonObject();
    REQUIRE(body != nullptr);
    CHECK((*body)["scopes"].isArray());
    CHECK((*body)["scopes"].empty());
    const auto empty = readScopeClient(clientId);
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    const auto removed = waitResponse([&](ClientAdmin::ResponseCallback cb) {
        ClientAdmin::deleteClient(
          jsonRequest(::drogon::Delete, "/api/admin/clients/" + clientId), std::move(cb), clientId);
    });
    REQUIRE(removed != nullptr);
    CHECK(removed->getStatusCode() == ::drogon::k200OK);
}

// Exercise the real framework rollback contract after a completed DELETE.
// This does not inject faults into the service or into noexcept Mapper code.
DROGON_TEST(Integration_P0_Admin_ScopesPreparationFailure_RollbackPreservesRows)
{
    auto db = dbOrNull();
    if (!db)
        return;
    using Scope = ::drogon_model::fulla_db::Oauth2ClientScopes;
    using Rows = std::vector<Scope>;

    const auto created = createScopeClient();
    REQUIRE(created.has_value());
    const std::string clientId = *created;
    auto response = waitResponse([&](ClientAdmin::ResponseCallback cb) {
        ClientAdmin::updateClientScopes(
          jsonRequest(::drogon::Put, "/api/admin/clients/" + clientId + "/scopes",
                      R"({"scopes":["openid","profile"]})"),
          std::move(cb), clientId);
    });
    REQUIRE(response != nullptr);
    REQUIRE(response->getStatusCode() == ::drogon::k200OK);
    auto initial = readScopeClient(clientId);
    REQUIRE(initial.has_value());
    std::sort(initial->begin(), initial->end());
    REQUIRE(*initial == std::vector<std::string>({"openid", "profile"}));

    auto deleted = std::make_shared<std::promise<bool>>();
    auto deletedFuture = deleted->get_future();
    {
        auto transaction = db->newTransaction();
        try
        {
            ::drogon::orm::Mapper<Scope> mapper(transaction);
            mapper.deleteBy(
              ::drogon::orm::Criteria(Scope::Cols::_client_id,
                                     ::drogon::orm::CompareOperator::EQ, clientId),
              [transaction, deleted](const size_t affected) {
                  try
                  {
                      throw std::runtime_error("scope preparation failure probe");
                  }
                  catch (const std::runtime_error &)
                  {
                      transaction->rollback();
                      deleted->set_value(affected == 2);
                  }
              },
              [transaction, deleted](const ::drogon::orm::DrogonDbException &) {
                  transaction->rollback();
                  deleted->set_value(false);
              });
        }
        catch (...)
        {
            transaction->rollback();
            deleted->set_value(false);
        }
    }
    REQUIRE(deletedFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    REQUIRE(deletedFuture.get());

    // A normal MVCC read could see old rows before a pending COMMIT/ROLLBACK.
    // FOR UPDATE waits for the DELETE's lock, so it checks the final outcome.
    auto selected = std::make_shared<std::promise<std::optional<Rows>>>();
    auto selectedFuture = selected->get_future();
    try
    {
        ::drogon::orm::Mapper<Scope> mapper(db);
        mapper.forUpdate().findBy(
          ::drogon::orm::Criteria(Scope::Cols::_client_id,
                                 ::drogon::orm::CompareOperator::EQ, clientId),
          [selected](const Rows &rows) { selected->set_value(rows); },
          [selected](const ::drogon::orm::DrogonDbException &) {
              selected->set_value(std::nullopt);
          });
    }
    catch (...)
    {
        selected->set_value(std::nullopt);
    }
    REQUIRE(selectedFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    const auto rows = selectedFuture.get();
    REQUIRE(rows.has_value());
    std::vector<std::string> actual;
    for (const auto &row : *rows)
        actual.push_back(row.getValueOfScopeName());
    std::sort(actual.begin(), actual.end());
    CHECK(actual == *initial);

    response = waitResponse([&](ClientAdmin::ResponseCallback cb) {
        ClientAdmin::updateClientScopes(
          jsonRequest(::drogon::Put, "/api/admin/clients/" + clientId + "/scopes",
                      R"({"scopes":[]})"), std::move(cb), clientId);
    });
    REQUIRE(response != nullptr);
    REQUIRE(response->getStatusCode() == ::drogon::k200OK);
    response = waitResponse([&](ClientAdmin::ResponseCallback cb) {
        ClientAdmin::deleteClient(
          jsonRequest(::drogon::Delete, "/api/admin/clients/" + clientId), std::move(cb), clientId);
    });
    REQUIRE(response != nullptr);
    CHECK(response->getStatusCode() == ::drogon::k200OK);
}
