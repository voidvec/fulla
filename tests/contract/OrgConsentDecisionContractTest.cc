#include "ContractFixtures.h"
#include "../common/HttpTestClient.h"
#include "openplatform/ApplicationService.h"

#include <fulla/storage/postgres/AdvisoryLock.h>
#include <fulla/storage/postgres/OrgConsentRepository.h>
#include <fulla/storage/postgres/OrgConsentRequestRepository.h>
#include <fulla/storage/postgres/models/Organizations.h>
#include <fulla/storage/postgres/models/OrganizationMembers.h>
#include <fulla/storage/postgres/models/OrganizationConsents.h>
#include <fulla/storage/postgres/models/Oauth2Clients.h>
#include <fulla/storage/postgres/models/Oauth2ClientOwners.h>
#include <fulla/storage/postgres/models/Users.h>
#include <drogon/orm/Mapper.h>

#include <atomic>
#include <future>

namespace
{
using namespace ::drogon::orm;
using namespace ::drogon_model::fulla_db;
using namespace fulla::storage::postgres;
using namespace fulla::test::contract;
namespace http = fulla::test::http;
using Status = OrgConsentDecisionStatus;
using Txn = std::shared_ptr<Transaction>;

template <typename T, typename Operation>
T await(Operation operation)
{
    auto promise = std::make_shared<std::promise<T>>();
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto future = promise->get_future();
    operation([promise, calls](T value) {
        if (calls->fetch_add(1) == 0)
            promise->set_value(std::move(value));
    });
    if (future.wait_for(std::chrono::seconds(15)) != std::future_status::ready)
        throw std::runtime_error("S04 callback timed out");
    return future.get();
}

template <typename Row>
Row insert(const DbClientPtr &db, const Row &row)
{
    auto result = await<std::optional<Row>>([&](auto cb) {
        try
        {
            Mapper<Row> mapper(db);
            mapper.insert(
              row,
              [cb](const Row &saved) { cb(saved); },
              [cb](const DrogonDbException &) { cb(std::nullopt); }
            );
        }
        catch (...)
        {
            cb(std::nullopt);
        }
    });
    if (!result)
        throw std::runtime_error("S04 fixture insert failed");
    return *result;
}

template <typename Row>
std::vector<Row> find(const DbClientPtr &db, const Criteria &criteria)
{
    auto rows = await<std::optional<std::vector<Row>>>([&](auto cb) {
        try
        {
            Mapper<Row> mapper(db);
            mapper.findBy(
              criteria,
              [cb](const std::vector<Row> &r) { cb(r); },
              [cb](const DrogonDbException &) { cb(std::nullopt); }
            );
        }
        catch (...)
        {
            cb(std::nullopt);
        }
    });
    if (!rows)
        throw std::runtime_error("S04 fixture lookup failed");
    return *rows;
}

template <typename Row>
size_t erase(const DbClientPtr &db, const Criteria &criteria)
{
    return await<size_t>([&](auto cb) {
        try
        {
            Mapper<Row> mapper(db);
            mapper.deleteBy(
              criteria,
              [cb](size_t count) { cb(count); },
              [cb](const DrogonDbException &) { cb(0); }
            );
        }
        catch (...)
        {
            cb(0);
        }
    });
}

template <typename Row>
size_t update(const DbClientPtr &db, const Row &row)
{
    return await<size_t>([&](auto cb) {
        try
        {
            Mapper<Row> mapper(db);
            mapper.update(
              row, [cb](size_t count) { cb(count); }, [cb](const DrogonDbException &) { cb(0); }
            );
        }
        catch (...)
        {
            cb(0);
        }
    });
}

struct Fixture
{
    DbClientPtr db;
    std::string bearer;
    std::string client;
    Organizations org;
    OrganizationMembers manager;
    Users requester;
    int32_t actor = 0;
    int32_t request = 0;
    int32_t sibling = 0;

    Fixture() : db(getPostgresClientOrNull())
    {
        auto token = http::loginAsAdmin();
        if (!token)
            throw std::runtime_error("S04 fixture admin login failed");
        bearer = *token;
        const auto users =
          find<Users>(db, Criteria(Users::Cols::_username, CompareOperator::EQ, "admin"));
        if (users.empty())
            throw std::runtime_error("S04 fixture requires seeded admin");
        actor = users.front().getValueOfId();
        const auto suffix = uniqueSuffix();
        // Create clients through the public API, never hand-written INSERT.
        Json::Value body;
        body["name"] = "s04-" + suffix;
        body["redirect_uris"] = "http://localhost:9999/callback";
        body["allowed_grant_types"] = "authorization_code";
        body["client_type"] = "CONFIDENTIAL";
        auto response = http::sendPostJson("/api/admin/clients", body, bearer);
        Json::Value created;
        if (
          !http::statusIs(response, ::drogon::k201Created) ||
          !http::parseJsonBody(response, created)
        )
            throw std::runtime_error("S04 fixture client creation failed");
        client = created["client_id"].asString();
        setScopes({"openid", "org", "profile"});
        Organizations row;
        auto slug = "s04-" + suffix;
        std::replace(slug.begin(), slug.end(), '_', '-');
        row.setSlug(slug);
        row.setName("S04 contract " + suffix);
        org = insert(db, row);
        OrganizationMembers member;
        member.setOrganizationId(org.getValueOfId());
        member.setUserId(actor);
        member.setRole("owner");
        manager = insert(db, member);
        Users second;
        second.setUsername("s04_" + suffix);
        second.setEmail("s04_" + suffix + "@example.test");
        second.setPasswordHash("unused-contract-fixture");
        second.setSalt("");
        requester = insert(db, second);
        member = OrganizationMembers{};
        member.setOrganizationId(org.getValueOfId());
        member.setUserId(requester.getValueOfId());
        member.setRole("member");
        insert(db, member);
        Oauth2ClientOwners owner;
        owner.setClientId(client);
        owner.setCreatorUserId(actor);
        owner.setStatus("active");
        insert(db, owner);
        OrgConsentRequestRepository requests(db);
        if (
          await<size_t>([&](auto cb) {
              requests.fileRequest(org.getValueOfId(), client, actor, std::move(cb));
          }) != 1 ||
          await<size_t>([&](auto cb) {
              requests
                .fileRequest(org.getValueOfId(), client, requester.getValueOfId(), std::move(cb));
          }) != 1
        )
            throw std::runtime_error("S04 fixture request filing failed");
        auto rows = find<OrganizationConsentRequests>(
          db,
          Criteria(
            OrganizationConsentRequests::Cols::_organization_id,
            CompareOperator::EQ,
            org.getValueOfId()
          )
        );
        for (const auto &r : rows)
            (r.getValueOfRequestedBy() == actor ? request : sibling) = r.getValueOfId();
        if (!request || !sibling)
            throw std::runtime_error("S04 fixture requests missing");
    }

    ~Fixture()
    {
        try
        {
            erase<Organizations>(
              db, Criteria(Organizations::Cols::_id, CompareOperator::EQ, org.getValueOfId())
            );
            erase<Oauth2Clients>(
              db, Criteria(Oauth2Clients::Cols::_client_id, CompareOperator::EQ, client)
            );
            erase<Users>(
              db, Criteria(Users::Cols::_id, CompareOperator::EQ, requester.getValueOfId())
            );
        }
        catch (...)
        {
        }
    }

    void setScopes(std::initializer_list<const char *> names)
    {
        Json::Value body;
        body["scopes"] = Json::Value(Json::arrayValue);
        for (const auto *name : names)
            body["scopes"].append(name);
        auto response = http::sendPutJson("/api/admin/clients/" + client + "/scopes", body, bearer);
        if (!http::statusIs(response, ::drogon::k200OK))
            throw std::runtime_error("S04 fixture scope update failed");
    }

    OrgConsentDecisionResult decide(
      bool approve = true,
      std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0)
    )
    {
        return await<OrgConsentDecisionResult>([&](auto cb) {
            OrgConsentRequestRepository(db).decideAtomically(
              org.getValueOfId(),
              request,
              actor,
              approve,
              "S04 reason",
              [cb, calls](OrgConsentDecisionResult result) {
                  ++*calls;
                  cb(std::move(result));
              }
            );
        });
    }

    std::vector<OrganizationConsents> active()
    {
        return find<OrganizationConsents>(
          db,
          Criteria(
            OrganizationConsents::Cols::_organization_id, CompareOperator::EQ, org.getValueOfId()
          ) &&
            Criteria(OrganizationConsents::Cols::_client_id, CompareOperator::EQ, client) &&
            Criteria(OrganizationConsents::Cols::_revoked_at, CompareOperator::IsNull)
        );
    }

    std::string status(int32_t id)
    {
        auto rows = find<OrganizationConsentRequests>(
          db, Criteria(OrganizationConsentRequests::Cols::_id, CompareOperator::EQ, id)
        );
        return rows.empty() ? "missing" : rows.front().getValueOfStatus();
    }

    size_t revoke()
    {
        return await<size_t>([&](auto cb) {
            OrgConsentRepository(db)
              .revokeClientConsents(org.getValueOfId(), client, std::move(cb));
        });
    }
};

// Test-only token-specific DDL. No production schema/model is changed.
struct Trigger
{
    DbClientPtr db;
    std::string name = "s04_" + uniqueSuffix();
    std::string table;

    Trigger(
      const DbClientPtr &client,
      std::string target,
      const std::string &timing,
      const std::string &predicate,
      const std::string &body,
      bool deferred = false
    )
        : db(client), table(std::move(target))
    {
        db->execSqlSync(
          "CREATE FUNCTION " + name + "() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN " + body +
          " RETURN NEW; END; $$"
        );
        db->execSqlSync(
          std::string("CREATE ") + (deferred ? "CONSTRAINT " : "") + "TRIGGER " + name + " " +
          timing + " ON " + table + (deferred ? " DEFERRABLE INITIALLY DEFERRED" : "") +
          " FOR EACH ROW WHEN (" + predicate + ") EXECUTE FUNCTION " + name + "()"
        );
    }

    ~Trigger()
    {
        try
        {
            db->execSqlSync("DROP TRIGGER IF EXISTS " + name + " ON " + table);
            db->execSqlSync("DROP FUNCTION IF EXISTS " + name + "()");
        }
        catch (...)
        {
        }
    }
};

Txn hold(const DbClientPtr &db, const std::string &key)
{
    return await<Txn>([&](auto cb) {
        withAdvisoryXactLock(
          db, {key}, [cb](const Txn &txn) { cb(txn); }, [cb](const std::string &) { cb(nullptr); }
        );
    });
}

bool waiting(const DbClientPtr &db, const std::string &key, int count = 1)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    do
    {
        // Test-only observation of the advisory concurrency primitive.
        auto rows = db->execSqlSync(
          "SELECT count(*) AS n FROM pg_locks WHERE locktype='advisory' "
          "AND objid=(hashtext($1)::bigint & 4294967295)::oid AND NOT granted",
          key
        );
        if (rows[0]["n"].as<int>() >= count)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
}  // namespace

DROGON_TEST(Integration_P1_OrgConsentAtomicity_ScopeFailuresRollBackEverything)
{
    if (!getPostgresClientOrNull())
        return;
    Fixture f;
    OrganizationConsents old;
    old.setOrganizationId(f.org.getValueOfId());
    old.setClientId(f.client);
    old.setScopeName("email");
    old.setGrantedBy(f.actor);
    insert(f.db, old);
    OrganizationConsents previous;
    previous.setOrganizationId(f.org.getValueOfId());
    previous.setClientId(f.client);
    previous.setScopeName("openid");
    previous.setGrantedBy(f.requester.getValueOfId());
    previous.setGrantedAt(::trantor::Date(123000000));
    previous.setRevokedAt(::trantor::Date(234000000));
    previous = insert(f.db, previous);
    for (const auto *scope : {"openid", "org", "profile"})
    {
        Trigger fault(
          f.db,
          "organization_consents",
          "BEFORE INSERT",
          "NEW.client_id='" + f.client + "' AND NEW.scope_name='" + scope + "'",
          "RAISE EXCEPTION 'S04 scope rejection';"
        );
        auto calls = std::make_shared<std::atomic<int>>(0);
        CHECK(f.decide(true, calls).status == Status::StorageError);
        CHECK(calls->load() == 1);
        auto rows = f.active();
        REQUIRE(rows.size() == 1);
        CHECK(rows.front().getValueOfScopeName() == "email");
        const auto restored = find<OrganizationConsents>(
          f.db,
          Criteria(OrganizationConsents::Cols::_id, CompareOperator::EQ, previous.getValueOfId())
        );
        REQUIRE(restored.size() == 1);
        CHECK(restored.front().getValueOfGrantedBy() == previous.getValueOfGrantedBy());
        CHECK(restored.front().getValueOfGrantedAt() == previous.getValueOfGrantedAt());
        CHECK(restored.front().getRevokedAt() != nullptr);
        CHECK(restored.front().getValueOfRevokedAt() == previous.getValueOfRevokedAt());
        CHECK(f.status(f.request) == "pending");
        CHECK(f.status(f.sibling) == "pending");
    }
}

DROGON_TEST(Integration_P1_OrgConsentAtomicity_StateAndSiblingFailuresRollBack)
{
    if (!getPostgresClientOrNull())
        return;
    Fixture f;
    for (int32_t id : {f.request, f.sibling})
    {
        Trigger fault(
          f.db,
          "organization_consent_requests",
          "BEFORE UPDATE",
          "NEW.id=" + std::to_string(id),
          "RAISE EXCEPTION 'S04 state rejection';"
        );
        CHECK(f.decide().status == Status::StorageError);
        CHECK(f.active().empty());
        CHECK(f.status(f.request) == "pending");
        CHECK(f.status(f.sibling) == "pending");
    }
    // A successful SQL statement matching zero rows must roll back the
    // already inserted scopes, not fall through to destructor-time COMMIT.
    Trigger noUpdate(
      f.db,
      "organization_consent_requests",
      "BEFORE UPDATE",
      "NEW.id=" + std::to_string(f.request),
      "RETURN NULL;"
    );
    CHECK(f.decide().status == Status::Conflict);
    CHECK(f.active().empty());
    CHECK(f.status(f.request) == "pending");
    CHECK(f.status(f.sibling) == "pending");
}

DROGON_TEST(Integration_P1_OrgConsentAtomicity_DeferredCommitFailureIsNotSuccess)
{
    if (!getPostgresClientOrNull())
        return;
    Fixture f;
    {
        Trigger fault(
          f.db,
          "organization_consent_requests",
          "AFTER UPDATE",
          "NEW.id=" + std::to_string(f.request),
          "RAISE EXCEPTION 'S04 commit rejection';",
          true
        );
        auto calls = std::make_shared<std::atomic<int>>(0);
        CHECK(f.decide(true, calls).status == Status::StorageError);
        CHECK(f.active().empty());
        CHECK(f.status(f.request) == "pending");
        CHECK(f.status(f.sibling) == "pending");
        CHECK(calls->load() == 1);
    }
    CHECK(f.decide().status == Status::Approved);
    Trigger revokeFailure(
      f.db,
      "organization_consents",
      "AFTER UPDATE",
      "NEW.client_id='" + f.client + "' AND NEW.revoked_at IS NOT NULL",
      "RAISE EXCEPTION 'S04 revoke commit rejection';",
      true
    );
    CHECK(f.revoke() == 0);
    CHECK(f.active().size() == 3);
    CHECK(f.status(f.request) == "approved");
}

DROGON_TEST(Integration_P1_OrgConsentAtomicity_NoExpansionAndExplicitRepair)
{
    if (!getPostgresClientOrNull())
        return;
    Fixture f;
    CHECK(f.decide().status == Status::Approved);
    CHECK(f.active().size() == 3);
    CHECK(f.status(f.sibling) == "approved");
    f.setScopes({"openid", "org", "profile", "email"});
    CHECK(f.decide().status == Status::AlreadyApproved);
    CHECK(f.active().size() == 3);
    // A legacy partial active grant is not permission to add current scopes.
    auto rows = f.active();
    for (size_t i = 1; i < rows.size(); ++i)
    {
        rows[i].setRevokedAt(::trantor::Date::now());
        CHECK(update(f.db, rows[i]) == 1);
    }
    CHECK(f.decide().status == Status::AlreadyApproved);
    CHECK(f.active().size() == 1);
    CHECK(f.revoke() == 1);
    CHECK(f.decide().status == Status::Approved);
    CHECK(f.active().size() == 4);
    CHECK(f.decide().status == Status::AlreadyApproved);
    CHECK(f.active().size() == 4);
    CHECK(
      erase<OrganizationMembers>(
        f.db,
        Criteria(OrganizationMembers::Cols::_id, CompareOperator::EQ, f.manager.getValueOfId())
      ) == 1
    );
    CHECK(f.revoke() == 4);
    CHECK(f.decide().status == Status::Forbidden);
    CHECK(f.active().empty());
}

DROGON_TEST(Integration_P1_OrgConsentAtomicity_EmptyScopesAndRejectedConflict)
{
    if (!getPostgresClientOrNull())
        return;
    Fixture f;
    f.setScopes({});
    auto result = f.decide();
    CHECK(result.status == Status::Approved);
    CHECK(result.scopes.empty());
    CHECK(f.active().empty());
    CHECK(f.status(f.request) == "approved");
    CHECK(f.status(f.sibling) == "approved");
    CHECK(f.decide(false).status == Status::Conflict);
}

DROGON_TEST(Integration_P1_OrgConsentAtomicity_ConcurrentDecisionsSerialize)
{
    if (!getPostgresClientOrNull())
        return;
    for (const auto &order : {std::pair<bool, bool>{true, false}, {false, true}, {true, true}})
    {
        Fixture f;
        const auto key = "org:" + std::to_string(f.org.getValueOfId());
        auto lock = hold(f.db, key);
        REQUIRE(lock != nullptr);
        auto first =
          std::async(std::launch::async, [&f, order]() { return f.decide(order.first); });
        CHECK(waiting(f.db, key));
        auto second =
          std::async(std::launch::async, [&f, order]() { return f.decide(order.second); });
        CHECK(waiting(f.db, key, 2));
        lock.reset();
        auto a = first.get(), b = second.get();
        CHECK(a.status == (order.first ? Status::Approved : Status::Rejected));
        CHECK(
          b.status == (order.first && order.second ? Status::AlreadyApproved : Status::Conflict)
        );
        CHECK(f.active().size() == (order.first ? size_t{3} : size_t{0}));
        CHECK(f.status(f.request) == (order.first ? "approved" : "rejected"));
    }
}

DROGON_TEST(Integration_P1_OrgConsentAtomicity_RevokeAndWithdrawWaitForWholePair)
{
    if (!getPostgresClientOrNull())
        return;
    Fixture f;
    const auto gateKey = "s04-insert:" + uniqueSuffix();
    auto gate = hold(f.db, gateKey);
    REQUIRE(gate != nullptr);
    Trigger pause(
      f.db,
      "organization_consents",
      "BEFORE INSERT",
      "NEW.client_id='" + f.client + "' AND NEW.scope_name='profile'",
      "PERFORM pg_advisory_xact_lock(hashtext('" + gateKey + "'));"
    );
    auto approval = std::async(std::launch::async, [&f]() { return f.decide(); });
    CHECK(waiting(f.db, gateKey));
    CHECK(f.active().empty());
    auto revoke = std::async(std::launch::async, [&f]() { return f.revoke(); });
    CHECK(waiting(f.db, "orgconsent:" + std::to_string(f.org.getValueOfId()) + ":" + f.client));
    auto withdrawal = std::async(std::launch::async, [&f]() {
        return await<size_t>([&](auto cb) {
            OrgConsentRequestRepository(f.db)
              .withdrawOwnPending(f.org.getValueOfId(), f.request, f.actor, std::move(cb));
        });
    });
    bool deleteWaiting = false;
    const auto deleteQueuedBy = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    do
    {
        // The DELETE must actually be queued behind the locked request row,
        // rather than starting only after approval happened to commit.
        auto rows = f.db->execSqlSync(
          "SELECT count(*) AS n FROM pg_stat_activity WHERE datname=current_database() "
          "AND wait_event_type='Lock' AND query ILIKE '%delete%organization_consent_requests%'"
        );
        deleteWaiting = rows[0]["n"].as<int>() > 0;
        if (!deleteWaiting)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (!deleteWaiting && std::chrono::steady_clock::now() < deleteQueuedBy);
    CHECK(deleteWaiting);
    gate.reset();
    CHECK(approval.get().status == Status::Approved);
    CHECK(revoke.get() == 3);
    CHECK(withdrawal.get() == 0);
    CHECK(f.active().empty());
    CHECK(f.status(f.request) == "approved");
}

DROGON_TEST(Integration_P1_OrgConsentAtomicity_WithdrawBeforeDecisionLeavesNoGrant)
{
    if (!getPostgresClientOrNull())
        return;
    Fixture f;
    const auto key = "org:" + std::to_string(f.org.getValueOfId());
    auto lock = hold(f.db, key);
    REQUIRE(lock != nullptr);
    auto approval = std::async(std::launch::async, [&f]() { return f.decide(); });
    CHECK(waiting(f.db, key));
    CHECK(await<size_t>([&](auto cb) {
              OrgConsentRequestRepository(f.db)
                .withdrawOwnPending(f.org.getValueOfId(), f.request, f.actor, std::move(cb));
          }) == 1);
    lock.reset();
    CHECK(approval.get().status == Status::NotFound);
    CHECK(f.active().empty());
    CHECK(f.status(f.sibling) == "pending");
}

DROGON_TEST(Integration_P1_OrgConsentAtomicity_RechecksAuthorityAfterLockWait)
{
    if (!getPostgresClientOrNull())
        return;
    for (int change : {0, 1, 2})
    {
        Fixture f;
        const auto key = "org:" + std::to_string(f.org.getValueOfId());
        auto lock = hold(f.db, key);
        REQUIRE(lock != nullptr);
        auto approval = std::async(std::launch::async, [&f]() { return f.decide(); });
        CHECK(waiting(f.db, key));
        if (change == 0)
        {
            f.manager.setRole("member");
            CHECK(update(f.db, f.manager) == 1);
        }
        else if (change == 1)
            CHECK(
              erase<OrganizationMembers>(
                f.db,
                Criteria(
                  OrganizationMembers::Cols::_id, CompareOperator::EQ, f.manager.getValueOfId()
                )
              ) == 1
            );
        else
        {
            f.org.setDeletedAt(::trantor::Date::now());
            CHECK(update(f.db, f.org) == 1);
        }
        lock.reset();
        CHECK(approval.get().status == (change == 2 ? Status::NotFound : Status::Forbidden));
        CHECK(f.active().empty());
        CHECK(f.status(f.request) == "pending");
    }
}

DROGON_TEST(Integration_P1_OrgConsentAtomicity_HttpFailureAndRetryConfirmWholeCommit)
{
    if (!getPostgresClientOrNull())
        return;
    Fixture f;
    const auto path = "/api/me/organizations/" + f.org.getValueOfSlug() + "/consent-requests/" +
                      std::to_string(f.request) + "/approve";
    {
        Trigger fault(
          f.db,
          "organization_consents",
          "BEFORE INSERT",
          "NEW.client_id='" + f.client + "' AND NEW.scope_name='org'",
          "RAISE EXCEPTION 'S04 HTTP tail rejection';"
        );
        auto response = http::sendPostJson(path, Json::Value(Json::objectValue), f.bearer);
        CHECK(http::statusIs(response, ::drogon::k500InternalServerError));
        CHECK(f.active().empty());
        CHECK(f.status(f.request) == "pending");
    }
    auto response = http::sendPostJson(path, Json::Value(Json::objectValue), f.bearer);
    CHECK(http::statusIs(response, ::drogon::k200OK));
    CHECK(f.active().size() == 3);
    CHECK(f.status(f.request) == "approved");
    CHECK(f.status(f.sibling) == "approved");
}

DROGON_TEST(Integration_P1_ApplicationList_ParallelOwnerFailuresRespondOnce)
{
    if (!getPostgresClientOrNull())
        return;
    Fixture f;
    auto tableLock = await<Txn>([&](auto cb) {
        f.db->newTransactionAsync([cb](const Txn &txn) {
            if (!txn)
            {
                cb(nullptr);
                return;
            }
            txn->execSqlAsync(
              "LOCK TABLE oauth2_client_owners IN ACCESS EXCLUSIVE MODE",
              [txn, cb](const Result &) { cb(txn); },
              [cb](const DrogonDbException &) { cb(nullptr); }
            );
        });
    });
    REQUIRE(tableLock != nullptr);
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto promise = std::make_shared<std::promise<::drogon::HttpResponsePtr>>();
    auto future = promise->get_future();
    auto cb = std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(
      [calls, promise](const ::drogon::HttpResponsePtr &response) {
          if (calls->fetch_add(1) == 0)
              promise->set_value(response);
      }
    );
    auto request = ::drogon::HttpRequest::newHttpRequest();
    request->getAttributes()->insert("userId", std::to_string(f.actor));
    openplatform::ApplicationService::list(request, cb);
    bool bothWaiting = false;
    const auto observedBy = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do
    {
        auto rows = f.db->execSqlSync(
          "SELECT count(*) AS n FROM pg_locks "
          "WHERE relation='oauth2_client_owners'::regclass AND NOT granted"
        );
        bothWaiting = rows[0]["n"].as<int>() >= 2;
        if (!bothWaiting)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (!bothWaiting && std::chrono::steady_clock::now() < observedBy);
    CHECK(bothWaiting);
    CHECK(future.wait_for(std::chrono::seconds(6)) == std::future_status::ready);
    if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        CHECK(http::statusIs(future.get(), ::drogon::k500InternalServerError));
    // Keep the table locked until BOTH failed statements have left PG.
    const auto drainedBy = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do
    {
        auto rows = f.db->execSqlSync(
          "SELECT count(*) AS n FROM pg_locks "
          "WHERE relation='oauth2_client_owners'::regclass AND NOT granted"
        );
        if (rows[0]["n"].as<int>() == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < drainedBy);
    tableLock.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(calls->load() == 1);
    auto good = http::sendGet("/api/me/applications", f.bearer);
    CHECK(http::statusIs(good, ::drogon::k200OK));
}

DROGON_TEST(Unit_P1_OrgConsentAtomicity_MissingBackendFailsClosed)
{
    auto result = await<OrgConsentDecisionResult>([](auto cb) {
        OrgConsentRequestRepository(nullptr).decideAtomically(1, 1, 1, true, "", std::move(cb));
    });
    CHECK(result.status == Status::StorageError);
}
