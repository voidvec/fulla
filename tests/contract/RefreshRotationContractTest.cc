#include "ContractFixtures.h"

#include <fulla/storage/memory/MemoryTokenRepository.h>
#include <fulla/storage/postgres/PostgresTokenRepository.h>
#include <fulla/storage/postgres/AdvisoryLock.h>
#include <fulla/storage/postgres/models/Oauth2RefreshTokens.h>
#include <drogon/orm/Mapper.h>

#include <atomic>
#include <future>

namespace
{
using namespace fulla::test::contract;
using namespace fulla::oauth2::repository;
using fulla::oauth2::model::OAuth2AccessToken;
using fulla::oauth2::model::OAuth2RefreshToken;
using fulla::storage::memory::MemoryTokenRepository;
using fulla::storage::postgres::PostgresTokenRepository;

template <typename T, typename Op>
T awaitResult(Op op)
{
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();
    op([promise](T result) { promise->set_value(std::move(result)); });
    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
        throw std::runtime_error("refresh contract timed out");
    return future.get();
}

struct Pair
{
    OAuth2AccessToken at;
    OAuth2RefreshToken rt;
};

Pair pairFor(const std::string &suffix, const std::string &family)
{
    Pair result;
    result.at.token = "s03-at-" + suffix;
    result.at.clientId = "fulla-portal";
    result.at.userId = "contract-user";
    result.at.scope = "profile";
    result.at.expiresAt = nowSeconds() + 3600;
    result.at.issuer = "http://localhost:5555";
    result.at.audience = result.at.clientId;
    result.at.issuedAt = nowSeconds();
    result.rt.token = "s03-rt-" + suffix;
    result.rt.accessToken = result.at.token;
    result.rt.clientId = result.at.clientId;
    result.rt.userId = result.at.userId;
    result.rt.scope = result.at.scope;
    result.rt.expiresAt = nowSeconds() + 3600;
    result.rt.familyId = family;
    return result;
}

RefreshTokenResult inspect(
  const std::shared_ptr<ITokenRepository> &repo,
  const OAuth2RefreshToken &rt,
  const std::string &client = "fulla-portal"
)
{
    return awaitResult<RefreshTokenResult>([&](auto cb) {
        repo->inspectRefreshToken(rt.token, client, std::move(cb));
    });
}

RefreshTokenResult rotate(
  const std::shared_ptr<ITokenRepository> &repo,
  const OAuth2RefreshToken &old,
  const Pair &next
)
{
    return awaitResult<RefreshTokenResult>([&](auto cb) {
        repo->rotateRefreshToken(old, next.at, next.rt, std::move(cb));
    });
}

bool save(const std::shared_ptr<ITokenRepository> &repo, const Pair &pair)
{
    return awaitResult<bool>([&](auto cb) {
        repo->saveTokenPair(pair.at, pair.rt, std::move(cb));
    });
}

void bindings(
  const std::shared_ptr<::drogon::test::Case> &TEST_CTX,
  const std::shared_ptr<ITokenRepository> &repo
)
{
    auto old = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    CHECK(save(repo, old));
    CHECK(inspect(repo, old.rt, "wrong-client").status == RefreshTokenStatus::ClientMismatch);
    CHECK(inspect(repo, old.rt).status == RefreshTokenStatus::Ready);
    auto next = pairFor(uniqueSuffix(), old.rt.familyId);
    auto forged = old.rt;
    forged.clientId = "wrong-client";
    CHECK(rotate(repo, forged, next).status == RefreshTokenStatus::ClientMismatch);
    CHECK(inspect(repo, old.rt).status == RefreshTokenStatus::Ready);
    next.rt.familyId = "another-family";
    CHECK(rotate(repo, old.rt, next).status == RefreshTokenStatus::StorageError);
    CHECK(inspect(repo, old.rt).status == RefreshTokenStatus::Ready);
    next.rt.familyId = old.rt.familyId;
    CHECK(rotate(repo, old.rt, next).status == RefreshTokenStatus::Rotated);
    CHECK(inspect(repo, old.rt).status == RefreshTokenStatus::AlreadyUsed);
    CHECK(inspect(repo, old.rt, "wrong-client").status == RefreshTokenStatus::ClientMismatch);
    CHECK(inspect(repo, next.rt).status == RefreshTokenStatus::Ready);

    auto expired = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    expired.rt.expiresAt = nowSeconds();
    CHECK(save(repo, expired));
    CHECK(inspect(repo, expired.rt).status == RefreshTokenStatus::Expired);
    auto replacement = pairFor(uniqueSuffix(), expired.rt.familyId);
    CHECK(rotate(repo, expired.rt, replacement).status == RefreshTokenStatus::Expired);
    auto used = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    used.rt.expiresAt = nowSeconds() - 10;
    used.rt.revoked = true;
    CHECK(save(repo, used));
    CHECK(inspect(repo, used.rt).status == RefreshTokenStatus::AlreadyUsed);
    CHECK(inspect(repo, used.rt, "wrong-client").status == RefreshTokenStatus::ClientMismatch);
}

void rollback(
  const std::shared_ptr<::drogon::test::Case> &TEST_CTX,
  const std::shared_ptr<ITokenRepository> &repo
)
{
    auto old = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    CHECK(save(repo, old));
    for (bool collideAccess : {true, false})
    {
        auto next = pairFor(uniqueSuffix(), old.rt.familyId);
        if (collideAccess)
        {
            next.at.token = old.at.token;
            next.rt.accessToken = old.at.token;
        }
        else
            next.rt.token = old.rt.token;
        CHECK(rotate(repo, old.rt, next).status == RefreshTokenStatus::StorageError);
        CHECK(inspect(repo, old.rt).status == RefreshTokenStatus::Ready);
        if (collideAccess)
            CHECK(inspect(repo, next.rt).status == RefreshTokenStatus::NotFound);
        else
        {
            auto token = awaitResult<std::optional<OAuth2AccessToken>>([&](auto cb) {
                repo->getAccessToken(next.at.token, std::move(cb));
            });
            CHECK(!token.has_value());
        }
    }
    auto next = pairFor(uniqueSuffix(), old.rt.familyId);
    CHECK(rotate(repo, old.rt, next).status == RefreshTokenStatus::Rotated);
}

// Named to avoid the PPL Concurrency namespace: on MSVC an unqualified
// `concurrency` call resolves against it (C2872/C2882, see the windows
// build failure on 895bdfe1).
void concurrentRotationAndFamilyRevoke(
  const std::shared_ptr<::drogon::test::Case> &TEST_CTX,
  const std::shared_ptr<ITokenRepository> &repo
)
{
    auto old = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    CHECK(save(repo, old));
    auto nextA = pairFor(uniqueSuffix(), old.rt.familyId);
    auto nextB = pairFor(uniqueSuffix(), old.rt.familyId);
    auto gate = std::make_shared<std::promise<void>>();
    auto ready = gate->get_future().share();
    auto operation = [repo, old, ready](const Pair &next) {
        ready.wait();
        auto result = rotate(repo, old.rt, next);
        if (result.status == RefreshTokenStatus::AlreadyUsed)
        {
            const bool revoked = awaitResult<bool>([&](auto cb) {
                repo->revokeTokenFamilyChecked(old.rt.familyId, std::move(cb));
            });
            if (!revoked)
                throw std::runtime_error("concurrent family revoke failed");
        }
        return result;
    };
    auto a = std::async(std::launch::async, operation, nextA);
    auto b = std::async(std::launch::async, operation, nextB);
    gate->set_value();
    auto first = a.get();
    auto second = b.get();
    CHECK(
      (int(first.status == RefreshTokenStatus::Rotated) +
       int(second.status == RefreshTokenStatus::Rotated)) == 1
    );
    CHECK(
      (int(first.status == RefreshTokenStatus::AlreadyUsed) +
       int(second.status == RefreshTokenStatus::AlreadyUsed)) == 1
    );
    const auto &winner = first.status == RefreshTokenStatus::Rotated ? nextA : nextB;
    CHECK(inspect(repo, winner.rt).status == RefreshTokenStatus::AlreadyUsed);
    auto token = awaitResult<std::optional<OAuth2AccessToken>>([&](auto cb) {
        repo->getAccessToken(winner.at.token, std::move(cb));
    });
    CHECK((!token || token->revoked));
    auto later = pairFor(uniqueSuffix(), old.rt.familyId);
    CHECK(rotate(repo, winner.rt, later).status == RefreshTokenStatus::AlreadyUsed);

    // Reverse order: revoke before a previously inspected RT can rotate.
    auto before = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    CHECK(save(repo, before));
    CHECK(inspect(repo, before.rt).status == RefreshTokenStatus::Ready);
    CHECK(awaitResult<bool>([&](auto cb) {
        repo->revokeTokenFamilyChecked(before.rt.familyId, std::move(cb));
    }));
    auto after = pairFor(uniqueSuffix(), before.rt.familyId);
    CHECK(rotate(repo, before.rt, after).status == RefreshTokenStatus::AlreadyUsed);
    CHECK(inspect(repo, after.rt).status == RefreshTokenStatus::NotFound);
}

std::shared_ptr<PostgresTokenRepository> postgres()
{
    if (!getPostgresClientOrNull())
        return nullptr;
    auto repo = std::make_shared<PostgresTokenRepository>();
    repo->initFromConfig(Json::Value());
    return repo;
}

bool waitForAdvisoryWaiter(const ::drogon::orm::DbClientPtr &db, const std::string &key)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline)
    {
        // Test-only inspection of the advisory concurrency primitive, not
        // application CRUD. This is the observable interleaving barrier.
        auto rows = db->execSqlSync(
          "SELECT count(*) AS waiting FROM pg_locks WHERE locktype='advisory' "
          "AND objid=(hashtext($1)::bigint & 4294967295)::oid AND NOT granted",
          key
        );
        if (rows[0]["waiting"].as<int>() > 0)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void expiryCleanupRetainsFamilyReachability(
  const std::shared_ptr<::drogon::test::Case> &TEST_CTX,
  const std::shared_ptr<ITokenRepository> &repo
)
{
    auto old = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    old.rt.expiresAt = nowSeconds() - 10;
    CHECK(save(repo, old));
    auto control = pairFor(uniqueSuffix(), "s03-control-" + uniqueSuffix());
    control.at.expiresAt = nowSeconds() - 10;
    control.rt.expiresAt = control.at.expiresAt;
    CHECK(save(repo, control));
    auto revoked = pairFor(uniqueSuffix(), "s03-revoked-" + uniqueSuffix());
    revoked.at.revoked = true;
    revoked.rt.expiresAt = nowSeconds() - 10;
    CHECK(save(repo, revoked));
    auto orphan = old.rt;
    orphan.token = "s03-orphan-" + uniqueSuffix();
    orphan.accessToken = "s03-missing-" + uniqueSuffix();
    CHECK(awaitResult<bool>([&](auto cb) {
        repo->saveRefreshToken(orphan, [cb]() { cb(true); });
    }));
    CHECK(inspect(repo, orphan).status == RefreshTokenStatus::Expired);
    repo->purgeExpired();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool swept = false;
    do
    {
        swept = inspect(repo, control.rt).status == RefreshTokenStatus::NotFound;
        if (swept)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    CHECK(swept);
    CHECK(inspect(repo, revoked.rt).status == RefreshTokenStatus::NotFound);
    CHECK(inspect(repo, orphan).status == RefreshTokenStatus::NotFound);
    // The control proves the asynchronous sweep finished. Expired metadata
    // must still connect the other live access token to its family.
    CHECK(inspect(repo, old.rt).status == RefreshTokenStatus::Expired);
    CHECK(awaitResult<bool>([&](auto cb) {
        repo->revokeTokenFamilyChecked(old.rt.familyId, std::move(cb));
    }));
    auto token = awaitResult<std::optional<OAuth2AccessToken>>([&](auto cb) {
        repo->getAccessToken(old.at.token, std::move(cb));
    });
    CHECK((!token || token->revoked));
    repo->purgeExpired();
    const auto reclaimedBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool reclaimed = false;
    do
    {
        reclaimed = inspect(repo, old.rt).status == RefreshTokenStatus::NotFound;
        if (reclaimed)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < reclaimedBy);
    CHECK(reclaimed);
}
}  // namespace

DROGON_TEST(Integration_P0_RefreshRotation_Memory_Bindings)
{
    bindings(TEST_CTX, std::make_shared<MemoryTokenRepository>());
}

DROGON_TEST(Integration_P0_RefreshRotation_Postgres_Bindings)
{
    auto repo = postgres();
    if (repo)
        bindings(TEST_CTX, repo);
}

DROGON_TEST(Integration_P0_RefreshRotation_Memory_PairFailureRollback)
{
    rollback(TEST_CTX, std::make_shared<MemoryTokenRepository>());
}

DROGON_TEST(Integration_P0_RefreshRotation_Postgres_PairFailureRollback)
{
    auto repo = postgres();
    if (repo)
        rollback(TEST_CTX, repo);
}

DROGON_TEST(Integration_P0_RefreshRotation_Memory_ConcurrentAndFamilyRevoke)
{
    concurrentRotationAndFamilyRevoke(TEST_CTX, std::make_shared<MemoryTokenRepository>());
}

DROGON_TEST(Integration_P0_RefreshRotation_Postgres_ConcurrentAndFamilyRevoke)
{
    auto repo = postgres();
    if (repo)
        concurrentRotationAndFamilyRevoke(TEST_CTX, repo);
}

DROGON_TEST(Integration_P0_RefreshRotation_Postgres_DeferredCommitRejection)
{
    auto repo = postgres();
    if (!repo)
        return;
    auto db = getPostgresClientOrNull();
    auto old = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    CHECK(save(repo, old));
    auto next = pairFor(uniqueSuffix(), old.rt.familyId);
    const auto name = "s03_commit_" + uniqueSuffix();
    // Test-only DDL: fail at COMMIT after both INSERTs succeeded. Unique
    // trigger/function names and a token-specific WHEN isolate other tests.
    db->execSqlSync(
      "CREATE FUNCTION " + name +
      "() RETURNS trigger LANGUAGE plpgsql AS $$ "
      "BEGIN RAISE EXCEPTION 'S03 deferred rejection'; END; $$"
    );

    struct Cleanup
    {
        ::drogon::orm::DbClientPtr db;
        std::string name;

        ~Cleanup()
        {
            try
            {
                db->execSqlSync("DROP TRIGGER IF EXISTS " + name + " ON oauth2_refresh_tokens");
                db->execSqlSync("DROP FUNCTION IF EXISTS " + name + "()");
            }
            catch (...)
            {
            }
        }
    } cleanup{db, name};

    db->execSqlSync(
      "CREATE CONSTRAINT TRIGGER " + name +
      " AFTER INSERT ON oauth2_refresh_tokens DEFERRABLE INITIALLY DEFERRED "
      "FOR EACH ROW WHEN (NEW.token = '" +
      next.rt.token + "') EXECUTE FUNCTION " + name + "()"
    );
    CHECK(rotate(repo, old.rt, next).status == RefreshTokenStatus::StorageError);
    CHECK(inspect(repo, old.rt).status == RefreshTokenStatus::Ready);
    CHECK(inspect(repo, next.rt).status == RefreshTokenStatus::NotFound);
    auto token = awaitResult<std::optional<OAuth2AccessToken>>([&](auto cb) {
        repo->getAccessToken(next.at.token, std::move(cb));
    });
    CHECK(!token.has_value());
}

DROGON_TEST(Integration_P0_RefreshRotation_Postgres_MissingBackendIsStorageError)
{
    auto repo = std::make_shared<PostgresTokenRepository>();
    auto old = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    CHECK(inspect(repo, old.rt).status == RefreshTokenStatus::StorageError);
    auto next = pairFor(uniqueSuffix(), old.rt.familyId);
    CHECK(rotate(repo, old.rt, next).status == RefreshTokenStatus::StorageError);
}

DROGON_TEST(Integration_P0_RefreshRotation_Postgres_ExpiryWhileWaitingForFamilyLock)
{
    auto repo = postgres();
    if (!repo)
        return;
    auto db = getPostgresClientOrNull();
    auto old = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    CHECK(save(repo, old));
    auto locked = std::make_shared<std::promise<std::shared_ptr<::drogon::orm::Transaction>>>();
    auto acquired = locked->get_future();
    fulla::storage::postgres::withAdvisoryXactLock(
      db,
      {"refresh-family:" + old.rt.familyId},
      [locked](const auto &txn) { locked->set_value(txn); },
      [locked](const std::string &) {
          locked->set_exception(std::make_exception_ptr(std::runtime_error("lock failed")));
      }
    );
    if (acquired.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
        throw std::runtime_error("lock acquisition timed out");
    auto hold = acquired.get();
    auto next = pairFor(uniqueSuffix(), old.rt.familyId);
    auto pending =
      std::async(std::launch::async, [repo, old, next]() { return rotate(repo, old.rt, next); });
    try
    {
        const bool waiting = waitForAdvisoryWaiter(db, "refresh-family:" + old.rt.familyId);
        CHECK(waiting);
        if (waiting)
        {
            // The authoritative row becomes expired while rotation waits.
            // Do not sleep past the test harness's three-second lock timeout.
            using Row = drogon_model::fulla_db::Oauth2RefreshTokens;
            Row expired;
            expired.setToken(old.rt.token);
            expired.setExpiresAt(nowSeconds());
            CHECK(awaitResult<size_t>([db, expired](auto cb) {
                      try
                      {
                          ::drogon::orm::Mapper<Row> mapper(db);
                          mapper.update(
                            expired,
                            [cb](size_t n) { cb(n); },
                            [cb](const ::drogon::orm::DrogonDbException &) { cb(0); }
                          );
                      }
                      catch (const std::exception &)
                      {
                          cb(0);
                      }
                      catch (...)
                      {
                          cb(0);
                      }
                  }) == 1);
        }
    }
    catch (...)
    {
        hold.reset();
        throw;
    }
    hold.reset();
    CHECK(pending.get().status == RefreshTokenStatus::Expired);
    CHECK(inspect(repo, next.rt).status == RefreshTokenStatus::NotFound);
}

DROGON_TEST(Integration_P0_RefreshRotation_Postgres_RevokeWaitsForPairCommit)
{
    auto repo = postgres();
    if (!repo)
        return;
    auto db = getPostgresClientOrNull();
    auto old = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    CHECK(save(repo, old));
    auto next = pairFor(uniqueSuffix(), old.rt.familyId);
    const auto name = "s03_insert_" + uniqueSuffix();
    const auto gate = "s03-insert-gate:" + uniqueSuffix();
    // Test-only, token-specific DDL pauses after the old RT CAS and before
    // the new access INSERT. A separate advisory key controls the pause.
    db->execSqlSync(
      "CREATE FUNCTION " + name +
      "() RETURNS trigger LANGUAGE plpgsql AS $$ "
      "BEGIN PERFORM pg_advisory_xact_lock(hashtext('" +
      gate + "')); RETURN NEW; END; $$"
    );

    struct Cleanup
    {
        ::drogon::orm::DbClientPtr db;
        std::string name;

        ~Cleanup()
        {
            try
            {
                db->execSqlSync("DROP TRIGGER IF EXISTS " + name + " ON oauth2_access_tokens");
                db->execSqlSync("DROP FUNCTION IF EXISTS " + name + "()");
            }
            catch (...)
            {
            }
        }
    } cleanup{db, name};

    db->execSqlSync(
      "CREATE TRIGGER " + name +
      " BEFORE INSERT ON oauth2_access_tokens FOR EACH ROW WHEN (NEW.token='" + next.at.token +
      "') EXECUTE FUNCTION " + name + "()"
    );
    auto hold = awaitResult<std::shared_ptr<::drogon::orm::Transaction>>([&](auto cb) {
        fulla::storage::postgres::withAdvisoryXactLock(
          db, {gate}, [cb](const auto &txn) { cb(txn); }, [cb](const std::string &) { cb(nullptr); }
        );
    });
    if (!hold)
        throw std::runtime_error("insert gate acquisition failed");
    std::future<RefreshTokenResult> rotation;
    std::future<bool> revocation;
    try
    {
        rotation = std::async(std::launch::async, [repo, old, next]() {
            return rotate(repo, old.rt, next);
        });
        const bool insideInsert = waitForAdvisoryWaiter(db, gate);
        CHECK(insideInsert);
        if (!insideInsert)
        {
            hold.reset();
            rotation.get();
            return;
        }
        revocation = std::async(std::launch::async, [repo, old]() {
            return awaitResult<bool>([&](auto cb) {
                repo->revokeTokenFamilyChecked(old.rt.familyId, std::move(cb));
            });
        });
        const bool revokeWaiting = waitForAdvisoryWaiter(db, "refresh-family:" + old.rt.familyId);
        CHECK(revokeWaiting);
        CHECK(revocation.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        hold.reset();
        CHECK(rotation.get().status == RefreshTokenStatus::Rotated);
        CHECK(revocation.get());
        CHECK(inspect(repo, next.rt).status == RefreshTokenStatus::AlreadyUsed);
        auto access = awaitResult<std::optional<OAuth2AccessToken>>([&](auto cb) {
            repo->getAccessToken(next.at.token, std::move(cb));
        });
        CHECK((!access || access->revoked));
    }
    catch (...)
    {
        hold.reset();
        throw;
    }
}

DROGON_TEST(Integration_P0_RefreshRotation_Memory_CleanupRetainsFamilyReachability)
{
    expiryCleanupRetainsFamilyReachability(TEST_CTX, std::make_shared<MemoryTokenRepository>());
}

DROGON_TEST(Integration_P0_RefreshRotation_Postgres_CleanupRetainsFamilyReachability)
{
    auto repo = postgres();
    if (repo)
        expiryCleanupRetainsFamilyReachability(TEST_CTX, repo);
}

DROGON_TEST(Integration_P0_RefreshRotation_Postgres_FamilyCommitRejectionRollsBackBothTables)
{
    auto repo = postgres();
    if (!repo)
        return;
    auto db = getPostgresClientOrNull();
    auto pair = pairFor(uniqueSuffix(), "s03-family-" + uniqueSuffix());
    CHECK(save(repo, pair));
    const auto name = "s03_revoke_" + uniqueSuffix();
    // Test-only deferred failure verifies that checked family revocation
    // does not report success after either UPDATE but before COMMIT.
    db->execSqlSync(
      "CREATE FUNCTION " + name +
      "() RETURNS trigger LANGUAGE plpgsql AS $$ "
      "BEGIN RAISE EXCEPTION 'S03 family commit rejected'; END; $$"
    );

    struct Cleanup
    {
        ::drogon::orm::DbClientPtr db;
        std::string name;

        ~Cleanup()
        {
            try
            {
                db->execSqlSync("DROP TRIGGER IF EXISTS " + name + " ON oauth2_access_tokens");
                db->execSqlSync("DROP FUNCTION IF EXISTS " + name + "()");
            }
            catch (...)
            {
            }
        }
    } cleanup{db, name};

    db->execSqlSync(
      "CREATE CONSTRAINT TRIGGER " + name +
      " AFTER UPDATE ON oauth2_access_tokens DEFERRABLE INITIALLY DEFERRED FOR EACH ROW "
      "WHEN (NEW.token='" +
      pair.at.token + "' AND NEW.revoked) EXECUTE FUNCTION " + name + "()"
    );
    CHECK(!awaitResult<bool>([&](auto cb) {
        repo->revokeTokenFamilyChecked(pair.rt.familyId, std::move(cb));
    }));
    CHECK(inspect(repo, pair.rt).status == RefreshTokenStatus::Ready);
    auto access = awaitResult<std::optional<OAuth2AccessToken>>([&](auto cb) {
        repo->getAccessToken(pair.at.token, std::move(cb));
    });
    CHECK((access && !access->revoked));
}
