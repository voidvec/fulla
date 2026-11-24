// tests/contract/UserInfoRepositoryContractTest.cc
//
// Spec: fulla-sdk-refactor -- 遗留事项 L2 (tasks.md): the dead
// tests/services/AuthServiceGetUserInfoTest.cc targeted the legacy
// fulla::drogon::services::AuthService::getUserInfo, which has NO
// remaining production caller (the live /oauth2/userinfo chain is
// TokenEndpointController -> OAuth2Plugin::getUserInfo ->
// fulla::identity::IUserRepository), and its `sub == numeric id`
// assertion conflicts with the V007 public_sub UUID model. Its genuinely
// uncovered behavior points -- role aggregation, name fallback (username
// empty -> email), empty roles array, not-found nullopt -- live in
// IUserRepository::getUserInfoWithRoles, so this file re-homes that
// coverage as contract tests over the two concrete implementations
// (Postgres/Memory), replacing the dead file.
//
// Fixture note (Postgres): users/roles/user_roles rows are seeded per run
// with collision-safe identifiers (uniqueSuffix) and deleted afterwards;
// user_roles rows also disappear via ON DELETE CASCADE (V005) when the
// user row is deleted, mirroring ConsentRepositoryContractTest.cc.
//
// Deliberate backend divergence (documented, not unified): Memory's
// getUserInfoWithRoles NEVER returns nullopt -- it synthesizes
// user_<id>/user_<id>@example.com claims and defaults roles to {"user"}
// (legacy memory-mode behavior) -- so the not-found/nullopt tier is
// Postgres-only here.

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include <fulla/storage/postgres/PostgresIdentityRepository.h>
#include <fulla/storage/memory/MemoryIdentityRepository.h>

#include "ContractFixtures.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fulla::test::contract;
using fulla::storage::memory::MemoryIdentityRepository;
using fulla::storage::postgres::PostgresIdentityRepository;

namespace
{

struct SeededUser
{
    int32_t id = -1;
    std::string publicSub;
};

// INSERT a throwaway users row; nullopt username -> SQL NULL username
// (allowed by the email-first model, see RegistrationWithoutUsernameTest);
// public_sub is filled by its gen_random_uuid() default (V007).
SeededUser seedUser(
  const ::drogon::orm::DbClientPtr &db,
  const std::optional<std::string> &username,
  const std::string &email
)
{
    return waitForValue<SeededUser>([&](auto cb) {
        auto onResult = [cb](const ::drogon::orm::Result &r) {
            SeededUser u;
            if (!r.empty())
            {
                u.id = r[0]["id"].as<int32_t>();
                u.publicSub = r[0]["public_sub"].as<std::string>();
            }
            cb(u);
        };
        auto onError = [cb](const ::drogon::orm::DrogonDbException &) { cb(SeededUser{}); };
        if (username)
        {
            db->execSqlAsync(
              "INSERT INTO users (username, password_hash, salt, email) "
              "VALUES ($1, $2, $3, $4) RETURNING id, public_sub::text AS public_sub",
              onResult,
              onError,
              *username,
              std::string("contract-test-hash"),
              std::string("contract-test-salt"),
              email
            );
        }
        else
        {
            db->execSqlAsync(
              "INSERT INTO users (password_hash, salt, email) "
              "VALUES ($1, $2, $3) RETURNING id, public_sub::text AS public_sub",
              onResult,
              onError,
              std::string("contract-test-hash"),
              std::string("contract-test-salt"),
              email
            );
        }
    });
}

int32_t seedRole(const ::drogon::orm::DbClientPtr &db, const std::string &roleName)
{
    return waitForValue<int32_t>([&](auto cb) {
        db->execSqlAsync(
          "INSERT INTO roles (name) VALUES ($1) RETURNING id",
          [cb](const ::drogon::orm::Result &r) { cb(r.empty() ? -1 : r[0]["id"].as<int32_t>()); },
          [cb](const ::drogon::orm::DrogonDbException &) { cb(-1); },
          roleName
        );
    });
}

bool linkUserRole(const ::drogon::orm::DbClientPtr &db, int32_t userId, int32_t roleId)
{
    return waitForValue<bool>([&](auto cb) {
        db->execSqlAsync(
          "INSERT INTO user_roles (user_id, role_id) VALUES ($1, $2)",
          [cb](const ::drogon::orm::Result &) { cb(true); },
          [cb](const ::drogon::orm::DrogonDbException &) { cb(false); },
          userId,
          roleId
        );
    });
}

// Deleting the user cascades the user_roles link (V005 ON DELETE CASCADE).
void cleanupUser(const ::drogon::orm::DbClientPtr &db, int32_t userId)
{
    waitForValue<bool>([&](auto cb) {
        db->execSqlAsync(
          "DELETE FROM users WHERE id = $1",
          [cb](const ::drogon::orm::Result &) { cb(true); },
          [cb](const ::drogon::orm::DrogonDbException &) { cb(false); },
          userId
        );
    });
}

void cleanupRole(const ::drogon::orm::DbClientPtr &db, int32_t roleId)
{
    waitForValue<bool>([&](auto cb) {
        db->execSqlAsync(
          "DELETE FROM roles WHERE id = $1",
          [cb](const ::drogon::orm::Result &) { cb(true); },
          [cb](const ::drogon::orm::DrogonDbException &) { cb(false); },
          roleId
        );
    });
}

std::optional<Json::Value> fetchUserInfo(
  const std::shared_ptr<PostgresIdentityRepository> &repo,
  int32_t userId
)
{
    return waitForValue<std::optional<Json::Value>>([&](auto cb) {
        repo->getUserInfoWithRoles(userId, std::move(cb));
    });
}

}  // namespace

// ===========================================================================
// Postgres
// ===========================================================================

DROGON_TEST(
  Integration_P0_Contract_Functional_UserInfoRepository_Postgres_UserWithRoles_ReturnsClaimsAndRoles
)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    const std::string suffix = uniqueSuffix();
    const std::string username = "contract_userinfo_" + suffix;
    const std::string email = username + "@example.com";
    const std::string roleName = "contract_userinfo_role_" + suffix;

    auto user = seedUser(db, username, email);
    REQUIRE(user.id > 0);
    REQUIRE(user.publicSub.empty() == false);
    int32_t roleId = seedRole(db, roleName);
    REQUIRE(roleId > 0);
    REQUIRE(linkUserRole(db, user.id, roleId) == true);

    auto repo = std::make_shared<PostgresIdentityRepository>(db);
    auto info = fetchUserInfo(repo, user.id);

    REQUIRE(info.has_value() == true);
    // sub is the public_sub UUID (V007), NOT the numeric internal id --
    // this is exactly the assertion the dead legacy test got wrong.
    CHECK((*info)["sub"].asString() == user.publicSub);
    CHECK((*info)["name"].asString() == username);
    CHECK((*info)["email"].asString() == email);
    REQUIRE((*info)["roles"].isArray() == true);
    REQUIRE((*info)["roles"].size() == 1u);
    CHECK((*info)["roles"][0].asString() == roleName);

    cleanupUser(db, user.id);
    cleanupRole(db, roleId);
}

DROGON_TEST(
  Integration_P0_Contract_Functional_UserInfoRepository_Postgres_NoRoles_EmptyRolesArray_NameFallsBackToEmail
)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    const std::string suffix = uniqueSuffix();
    const std::string email = "contract_userinfo_" + suffix + "@example.com";

    // NULL username: the name claim must fall back to the email.
    auto user = seedUser(db, std::nullopt, email);
    REQUIRE(user.id > 0);

    auto repo = std::make_shared<PostgresIdentityRepository>(db);
    auto info = fetchUserInfo(repo, user.id);

    REQUIRE(info.has_value() == true);
    CHECK((*info)["sub"].asString() == user.publicSub);
    CHECK((*info)["name"].asString() == email);
    CHECK((*info)["email"].asString() == email);
    // No user_roles rows -> roles present but EMPTY (graceful degradation
    // branch: user info survives even without any role rows).
    REQUIRE((*info)["roles"].isArray() == true);
    CHECK((*info)["roles"].size() == 0u);

    cleanupUser(db, user.id);
}

DROGON_TEST(
  Integration_P0_Contract_Functional_UserInfoRepository_Postgres_UnknownUser_ReturnsNullopt
)
{
    auto db = getPostgresClientOrNull();
    if (!db)
        return;

    auto repo = std::make_shared<PostgresIdentityRepository>(db);
    // SERIAL ids are positive; -1 can never exist.
    auto info = fetchUserInfo(repo, -1);
    CHECK(info.has_value() == false);
}

// ===========================================================================
// Memory
// ===========================================================================

DROGON_TEST(
  Integration_P0_Contract_Functional_UserInfoRepository_Memory_GetRoles_IntegerIdMatchesStringKey
)
{
    auto repo = std::make_shared<MemoryIdentityRepository>();
    Json::Value adminCfg;
    adminCfg["42"] = Json::Value(Json::arrayValue);
    adminCfg["42"].append("support");
    adminCfg["42"].append("billing");
    repo->initAdminRoles(adminCfg);

    const auto readRoles = [repo](const auto &key) {
        auto completion = std::make_shared<std::promise<std::vector<std::string>>>();
        auto result = completion->get_future();
        repo->getRoles(key, [completion](std::vector<std::string> roles) {
            completion->set_value(std::move(roles));
        });
        if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            throw std::runtime_error("Memory role query did not complete");
        return result.get();
    };
    const std::vector<std::string> configuredRoles{"support", "billing"};
    CHECK(readRoles(int32_t{42}) == configuredRoles);
    CHECK(readRoles(std::string("42")) == configuredRoles);
    const std::vector<std::string> defaultRoles{"user"};
    CHECK(readRoles(int32_t{43}) == defaultRoles);
}

DROGON_TEST(
  Integration_P0_Contract_Functional_UserInfoRepository_Memory_MappedUser_SyntheticClaimsWithConfiguredRoles
)
{
    MemoryIdentityRepository repo;
    Json::Value adminCfg;
    Json::Value roles(Json::arrayValue);
    roles.append("admin");
    roles.append("user");
    adminCfg["42"] = roles;
    repo.initAdminRoles(adminCfg);

    auto info = waitForValue<std::optional<Json::Value>>([&](auto cb) {
        repo.getUserInfoWithRoles(42, std::move(cb));
    });

    REQUIRE(info.has_value() == true);
    // Memory backend synthesizes claims from the numeric id (no user table).
    CHECK((*info)["sub"].asString() == "42");
    CHECK((*info)["name"].asString() == "user_42");
    CHECK((*info)["email"].asString() == "user_42@example.com");
    REQUIRE((*info)["roles"].isArray() == true);
    REQUIRE((*info)["roles"].size() == 2u);
    CHECK((*info)["roles"][0].asString() == "admin");
    CHECK((*info)["roles"][1].asString() == "user");
}

DROGON_TEST(
  Integration_P0_Contract_Functional_UserInfoRepository_Memory_UnmappedUser_DefaultsToUserRole_NeverNullopt
)
{
    // No initAdminRoles call at all: even a completely unknown id gets
    // synthetic claims + the {"user"} default role (documented divergence
    // from Postgres, which returns nullopt for unknown users).
    MemoryIdentityRepository repo;

    auto info = waitForValue<std::optional<Json::Value>>([&](auto cb) {
        repo.getUserInfoWithRoles(7, std::move(cb));
    });

    REQUIRE(info.has_value() == true);
    CHECK((*info)["sub"].asString() == "7");
    REQUIRE((*info)["roles"].isArray() == true);
    REQUIRE((*info)["roles"].size() == 1u);
    CHECK((*info)["roles"][0].asString() == "user");
}

// ===========================================================================
// Coverage additions (P1) -- Memory backend initAdminRoles parsing branches
// (null config -> default 'admin' user; roles as a string rather than an
// array) and the IUserRepository placeholder impls (synthetic findById,
// non-numeric findByPublicSub -> nullopt, the nullopt findByEmail/
// findByUsername, the unsupported create, and the no-op updatePasswordHash).
// ===========================================================================

// initAdminRoles: a null config seeds the default 'admin' user with
// {"admin","user"} (MemoryIdentityRepository.cc:36-40).
DROGON_TEST(Integration_P0_Contract_Functional_UserInfoRepository_Memory_InitAdminRoles_NullConfig_DefaultsAdminUser)
{
    MemoryIdentityRepository repo;
    repo.initAdminRoles(Json::Value());  // null -> default admin

    auto roles = waitForValue<std::vector<std::string>>([&](auto cb) {
        repo.getRoles(std::string("admin"), std::move(cb));
    });
    REQUIRE(roles.size() == 2u);
    CHECK(roles[0] == "admin");
    CHECK(roles[1] == "user");
}

// initAdminRoles: roles provided as a STRING (not an array) still register
// (MemoryIdentityRepository.cc:30-33).
DROGON_TEST(Integration_P0_Contract_Functional_UserInfoRepository_Memory_InitAdminRoles_RolesAsString)
{
    MemoryIdentityRepository repo;
    Json::Value adminCfg;
    adminCfg["string-role-user"] = "superadmin";  // string, not array
    repo.initAdminRoles(adminCfg);

    auto roles = waitForValue<std::vector<std::string>>([&](auto cb) {
        repo.getRoles(std::string("string-role-user"), std::move(cb));
    });
    REQUIRE(roles.size() == 1u);
    CHECK(roles[0] == "superadmin");
}

// findById: synthesizes a user record for any id (placeholder impl,
// MemoryIdentityRepository.cc:94-100).
DROGON_TEST(Integration_P0_Contract_Functional_UserInfoRepository_Memory_FindById_SyntheticUser)
{
    MemoryIdentityRepository repo;
    auto user = waitForValue<std::optional<fulla::identity::UserData>>([&](auto cb) {
        repo.findById(123, std::move(cb));
    });
    REQUIRE(user.has_value());
    CHECK(user->id == 123);
    CHECK(user->username == "user_123");
    CHECK(user->email == "user_123@example.com");
}

// findByPublicSub: a non-numeric public sub returns nullopt (stoi catch,
// MemoryIdentityRepository.cc:102-115).
DROGON_TEST(Integration_P0_Contract_Functional_UserInfoRepository_Memory_FindByPublicSub_NonNumeric_ReturnsNullopt)
{
    MemoryIdentityRepository repo;
    auto user = waitForValue<std::optional<fulla::identity::UserData>>([&](auto cb) {
        repo.findByPublicSub("not-a-number", std::move(cb));
    });
    CHECK(!user.has_value());
}

// findByPublicSub: a numeric public sub resolves to the synthetic user.
DROGON_TEST(Integration_P0_Contract_Functional_UserInfoRepository_Memory_FindByPublicSub_Numeric_SyntheticUser)
{
    MemoryIdentityRepository repo;
    auto user = waitForValue<std::optional<fulla::identity::UserData>>([&](auto cb) {
        repo.findByPublicSub("42", std::move(cb));
    });
    REQUIRE(user.has_value());
    CHECK(user->id == 42);
}

// findByEmail / findByUsername: placeholder impls always return nullopt
// (MemoryIdentityRepository.cc:117-131).
DROGON_TEST(Integration_P0_Contract_Functional_UserInfoRepository_Memory_FindByEmailAndUsername_ReturnNullopt)
{
    MemoryIdentityRepository repo;

    auto byEmail = waitForValue<std::optional<fulla::identity::UserData>>([&](auto cb) {
        repo.findByEmail("anyone@example.com", std::move(cb));
    });
    CHECK(!byEmail.has_value());

    auto byUsername = waitForValue<std::optional<fulla::identity::UserData>>([&](auto cb) {
        repo.findByUsername("anyone", std::move(cb));
    });
    CHECK(!byUsername.has_value());
}

// create: the memory backend cannot persist -> returns {nullopt, ""}
// (MemoryIdentityRepository.cc:133-141).
DROGON_TEST(Integration_P0_Contract_Functional_UserInfoRepository_Memory_Create_ReturnsNulloptUnsupported)
{
    MemoryIdentityRepository repo;
    fulla::identity::UserData ud;
    ud.username = "new";
    std::optional<int32_t> newId;
    std::string errCode = "unset";
    repo.create(ud, [&](std::optional<int32_t> id, std::string ec) {
        newId = id;
        errCode = ec;
    });
    CHECK(!newId.has_value());
    CHECK(errCode == "");
}

// updatePasswordHash: placeholder impl returns false (unsupported,
// MemoryIdentityRepository.cc:143-150). reset/incrementFailedLogins return
// true (fire-and-forget no-ops).
DROGON_TEST(Integration_P0_Contract_Functional_UserInfoRepository_Memory_PasswordAndLoginMethods_PlaceholderReturns)
{
    MemoryIdentityRepository repo;

    bool pwUpdated = true;
    repo.updatePasswordHash(1, "hash", [&](bool ok) { pwUpdated = ok; });
    CHECK(pwUpdated == false);

    bool reset = false;
    repo.resetFailedLogins(1, [&](bool ok) { reset = ok; });
    CHECK(reset == true);

    bool incremented = false;
    repo.incrementFailedLogins(1, [&](bool ok) { incremented = ok; });
    CHECK(incremented == true);
}
