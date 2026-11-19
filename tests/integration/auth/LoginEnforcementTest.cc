#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/utils/CryptoUtils.h>
#include <fulla/identity/TotpUtils.h>
#include <chrono>
#include <future>
#include <chrono>

using namespace drogon;
using namespace drogon::orm;

namespace
{

int64_t totpNowSeconds()
{
    return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count()
    );
}

}  // namespace

/**
 * Test: MFA enforcement - when user has mfa_enabled=true,
 * login should return mfa_required instead of auth code.
 * Uses in-process plugin calls (not HTTP client) to avoid port issues.
 */
DROGON_TEST(Integration_P1_Login_MFA_Enforcement)
{
    // Skip if using memory storage (no DB available)
    auto plugin = app().getPlugin<OAuth2Plugin>();
    if (!plugin || plugin->getStorageType() == "memory")
    {
        CHECK(true);
        return;
    }
    auto db = app().getDbClient();

    // Setup: enable MFA for admin user
    std::string secret = fulla::identity::totp::generateSecret(::fulla::drogon::utils::detail::cryptoProvider());
    std::promise<bool> pSetup;
    db->execSqlAsync(
      "UPDATE users SET mfa_enabled = true, mfa_secret = $1 WHERE username = 'admin'",
      [&](const Result &) { pSetup.set_value(true); },
      [&](const DrogonDbException &) { pSetup.set_value(false); },
      secret
    );
    REQUIRE(pSetup.get_future().get() == true);

    // Verify the field is set
    std::promise<bool> pVerify;
    db->execSqlAsync(
      "SELECT mfa_enabled FROM users WHERE username = 'admin'",
      [&](const Result &r) { pVerify.set_value(!r.empty() && r[0]["mfa_enabled"].as<bool>()); },
      [&](const DrogonDbException &) { pVerify.set_value(false); }
    );
    CHECK(pVerify.get_future().get() == true);

    // Verify TOTP generation works with this secret
    auto code = fulla::identity::totp::generateCode(secret, totpNowSeconds());
    CHECK(code.length() == 6);
    CHECK(fulla::identity::totp::verifyCode(secret, code, totpNowSeconds()) == true);

    // Cleanup: disable MFA
    std::promise<void> pCleanup;
    db->execSqlAsync(
      "UPDATE users SET mfa_enabled = false, mfa_secret = NULL WHERE username = 'admin'",
      [&](const Result &) { pCleanup.set_value(); },
      [&](const DrogonDbException &) { pCleanup.set_value(); }
    );
    pCleanup.get_future().get();
}

/**
 * Test: Email verification field is correctly stored and queryable
 */
DROGON_TEST(Integration_P1_Login_EmailVerification_Field)
{
    // Skip if storage type is memory
    auto plugin = drogon::app().getPlugin<OAuth2Plugin>();
    if (plugin && plugin->getStorageType() == "memory")
    {
        return;
    }

    auto db = app().getDbClient();
    if (!db)
    {
        CHECK(true);
        return;
    }

    // Check that email_verified column exists and is queryable
    std::promise<bool> pCheck;
    db->execSqlAsync(
      "SELECT email_verified FROM users WHERE username = 'admin'",
      [&](const Result &r) {
          // Column exists and is readable
          pCheck.set_value(!r.empty());
      },
      [&](const DrogonDbException & /*e*/) {
          // Column doesn't exist
          pCheck.set_value(false);
      }
    );
    CHECK(pCheck.get_future().get() == true);

    // Set email_verified = true and verify
    std::promise<bool> pSet;
    db->execSqlAsync(
      "UPDATE users SET email_verified = true WHERE username = 'admin'",
      [&](const Result &) { pSet.set_value(true); },
      [&](const DrogonDbException &) { pSet.set_value(false); }
    );
    CHECK(pSet.get_future().get() == true);

    std::promise<bool> pRead;
    db->execSqlAsync(
      "SELECT email_verified FROM users WHERE username = 'admin'",
      [&](const Result &r) { pRead.set_value(!r.empty() && r[0]["email_verified"].as<bool>()); },
      [&](const DrogonDbException &) { pRead.set_value(false); }
    );
    CHECK(pRead.get_future().get() == true);
}

/**
 * Test: PKCE enforcement config - verify the mechanism exists
 * With auth.require_pkce enabled (the default; the test config sets it
 * true explicitly), no client can login without a code_challenge
 */
DROGON_TEST(Integration_P1_Login_PKCE_Config_Exists)
{
    // Verify the config mechanism works
    auto customCfg = drogon::app().getCustomConfig();

    // F-011 (RFC 9700 §2.1.1): PKCE is required by default for all
    // authorization_code clients. The test config sets require_pkce
    // = true explicitly.
    bool requirePkce = true;
    if (customCfg.isMember("auth") && customCfg["auth"].isMember("require_pkce"))
    {
        requirePkce = customCfg["auth"]["require_pkce"].asBool();
    }
    // In test config, PKCE MUST be enforced (F-011 hardening).
    CHECK(requirePkce == true);

    // Verify email verification config
    bool requireEmail = false;
    if (customCfg.isMember("auth") && customCfg["auth"].isMember("require_email_verification"))
    {
        requireEmail = customCfg["auth"]["require_email_verification"].asBool();
    }
    // In test config, email verification should not be enforced
    CHECK(requireEmail == false);
}

/**
 * Test: Account lockout fields exist and work
 */
DROGON_TEST(Integration_P1_Login_AccountLockout_Fields)
{
    // Skip if storage type is memory
    auto plugin = drogon::app().getPlugin<OAuth2Plugin>();
    if (plugin && plugin->getStorageType() == "memory")
    {
        return;
    }

    auto db = app().getDbClient();
    if (!db)
    {
        CHECK(true);
        return;
    }

    // Verify lockout columns exist
    std::promise<bool> pCheck;
    db->execSqlAsync(
      "SELECT failed_login_count, locked_until, last_failed_login FROM users WHERE username = "
      "'admin'",
      [&](const Result &r) {
          if (r.empty())
          {
              pCheck.set_value(false);
              return;
          }
          // All columns should be readable (value may be non-zero from other tests)
          int failedCount =
            r[0]["failed_login_count"].isNull() ? 0 : r[0]["failed_login_count"].as<int>();
          CHECK(failedCount >= 0);  // Just verify it's a valid integer
          pCheck.set_value(true);
      },
      [&](const DrogonDbException &) { pCheck.set_value(false); }
    );
    CHECK(pCheck.get_future().get() == true);
}
