// tests/integration/admin/AdminBootstrapLegacyHashHttpTest.cc
//
// #103 retirement coverage:
//   1. AdminBootstrapper on an existing deployment (seeded 'admin') is an
//      idempotent no-op — the stored PBKDF2 credential is NOT overwritten
//      by a bootstrap run with a different explicit password.
//   2. First-boot creation path: with no user named 'admin' (simulated by
//      temporarily renaming the seeded one — FK children stay attached, so
//      restore is exact), the bootstrapper creates 'admin' with the env
//      password, hashed PBKDF2, and that credential logs in.
//   3. Legacy unsalted-SHA256 hashes are rejected at login with the window
//      closed (the assembly default since #103; the test server runs the
//      tracked config.json where allow_legacy_hash=false), WITHOUT
//      advancing the lockout counter (policy denial, not wrong password).
//
// All cases skip cleanly under memory storage / unreachable server.
#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>

#include "HttpTestClient.h"
#include "bootstrap/AdminBootstrapper.h"

#include <future>
#include <string>

using fulla::test::http::postgresAvailable;
using fulla::test::http::sendPostForm;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define ADMIN_BOOTSTRAP_SKIP_GUARD                                  \
    do                                                              \
    {                                                               \
        if (!postgresAvailable() || !serverReachable())             \
        {                                                           \
            CHECK(true);                                            \
            return;                                                 \
        }                                                           \
    } while (0)

namespace
{
// The retired dev-seed hash (sha256('admin' + 'admin_salt')) — kept HERE
// ONLY as the fixture for asserting it no longer verifies.
constexpr const char *kRetiredLegacyHash =
  "892738161086b314334f88d661aa6e7bab7c825c34bf55222811dad46cdbf724";

bool loginExpect(const std::string &username, const std::string &password, drogon::HttpStatusCode want)
{
    auto resp = sendPostForm(
      "/oauth2/login?json=true",
      "username=" + username + "&password=" + password +
        "&client_id=fulla-portal&redirect_uri=http%3A%2F%2F127.0.0.1%3A5173%2Fcallback"
        "&scope=openid&state=p0103&code_challenge=F_TTxId01kOTYIcFSCqZnz9wQ-6F1aJ1vtm1YoBy8po&code_challenge_method=S256"
    );
    return resp != nullptr && resp->getStatusCode() == want;
}

// RAII: temporarily rename 'admin' away so AdminBootstrapper takes the
// first-boot creation path; restores the exact original row on scope exit
// (children — roles, mappings — are untouched by a username rename).
class ScopedAdminRename
{
  public:
    explicit ScopedAdminRename(std::string backupName) : backup_(std::move(backupName))
    {
        // Move BOTH the username and the email aside: the bootstrapper's
        // insert carries admin@example.com, and the users email UNIQUE
        // constraint would reject it while the (renamed) original still
        // holds it — a genuinely fresh database has neither row.
        auto db = drogon::app().getDbClient();
        db->execSqlSync(
          "UPDATE users SET username = $1, email = $2 WHERE username = 'admin'",
          backup_, backup_ + "@example.test");
    }
    ~ScopedAdminRename()
    {
        if (!restored_)
        {
            auto db = drogon::app().getDbClient();
            if (db)
            {
                db->execSqlSync("DELETE FROM users WHERE username = 'admin'");
                db->execSqlSync(
                  "UPDATE users SET username = 'admin', email = 'admin@example.com' "
                  "WHERE username = $1",
                  backup_);
            }
        }
    }
    void restore()
    {
        auto db = drogon::app().getDbClient();
        db->execSqlSync("DELETE FROM users WHERE username = 'admin'");
        db->execSqlSync(
          "UPDATE users SET username = 'admin', email = 'admin@example.com' "
          "WHERE username = $1",
          backup_);
        restored_ = true;
    }

  private:
    std::string backup_;
    bool restored_ = false;
};
}  // namespace

// ---------------------------------------------------------------------------
// Existing 'admin' (seeded, PBKDF2): a bootstrap run with a DIFFERENT
// explicit password must not touch the stored credential.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminBootstrap_ExistingAdmin_IsIdempotentNoOp)
{
    ADMIN_BOOTSTRAP_SKIP_GUARD;

    auto done = std::make_shared<std::promise<bool>>();
    bootstrap::AdminBootstrapper::run(
      "DefinitelyNotTheSeededPw!123",
      [done](bool ok, const std::string &detail) {
          if (!ok)
              LOG_WARN << "bootstrap no-op test: " << detail;
          done->set_value(ok);
      }
    );
    CHECK(done->get_future().get());

    // The seeded credential still works — nothing was overwritten.
    CHECK(loginExpect("admin", "admin", drogon::k200OK));
}

// ---------------------------------------------------------------------------
// First-boot path: no user named 'admin' -> created with the env password,
// PBKDF2-hashed, and that exact credential logs in.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_AdminBootstrap_FreshAdmin_EnvPasswordCreatesPbkdf2User)
{
    ADMIN_BOOTSTRAP_SKIP_GUARD;

    const std::string envPw = "BootstrapPw!103x";
    {
        ScopedAdminRename guard("p0103bak_admin");

        auto done = std::make_shared<std::promise<bool>>();
        bootstrap::AdminBootstrapper::run(
          envPw,
          [done](bool ok, const std::string &detail) {
              if (!ok)
                  LOG_WARN << "bootstrap fresh test: " << detail;
              done->set_value(ok);
          }
        );
        CHECK(done->get_future().get());

        auto db = drogon::app().getDbClient();
        auto rows = db->execSqlSync(
          "SELECT password_hash FROM users WHERE username = 'admin'"
        );
        REQUIRE(rows.size() == 1);
        CHECK(std::string(rows[0]["password_hash"].as<std::string>())
                .find("$pbkdf2-sha256$") == 0);

        // The env credential works; the dev default does not.
        CHECK(loginExpect("admin", envPw, drogon::k200OK));
        CHECK(loginExpect("admin", "admin", drogon::k401Unauthorized));

        guard.restore();
    }
    // Original admin fully restored.
    CHECK(loginExpect("admin", "admin", drogon::k200OK));
}

// ---------------------------------------------------------------------------
// Legacy hash retirement: a user holding the retired unsalted-SHA256 hash
// is denied (generic 401, correct password included) and the denial does
// NOT advance the lockout counter — the policy rejection is not a wrong
// password, and an attacker must not be able to lock legacy users out.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P0_LegacyHash_LoginRejected_WindowClosedNoLockout)
{
    ADMIN_BOOTSTRAP_SKIP_GUARD;

    auto db = drogon::app().getDbClient();
    REQUIRE(db != nullptr);

    const std::string uname = "p0103legacy";
    db->execSqlSync(
      "INSERT INTO users (username, password_hash, salt, email) "
      "VALUES ($1, $2, 'admin_salt', 'p0103legacy@example.test') "
      "ON CONFLICT (username) DO UPDATE SET password_hash = $2, salt = 'admin_salt', "
      "failed_login_count = 0, locked_until = 0",
      uname, kRetiredLegacyHash
    );

    // Correct password for that hash ('admin') — still denied: the format
    // itself is rejected before verification.
    CHECK(loginExpect(uname, "admin", drogon::k401Unauthorized));
    // Repeated denial does not advance the counter.
    CHECK(loginExpect(uname, "admin", drogon::k401Unauthorized));

    auto rows = db->execSqlSync(
      "SELECT failed_login_count, locked_until FROM users WHERE username = $1", uname
    );
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["failed_login_count"].as<long>() == 0);
    CHECK(rows[0]["locked_until"].as<long>() == 0);

    // Test-data cleanup (pure test row; no children reference it).
    db->execSqlSync("DELETE FROM users WHERE username = $1", uname);
}
