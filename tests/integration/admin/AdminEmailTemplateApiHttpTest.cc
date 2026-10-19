// tests/integration/admin/AdminEmailTemplateApiHttpTest.cc
//
// P1-2 (#278): HTTP integration tests for the email-template admin API
// (libs/drogon/src/admin/EmailTemplateAdminService.cc +
// controllers/EmailTemplateAdminController.cc).
//
// Coverage: seeded list/get, save-time content-policy rejection, edit
// round-trip (restored at the end), test-send through the
// setEmailServiceForTesting spy seam (additive P1-2 factory override),
// recipient whitelist denial, and the rolling-24h quota gate.
//
// Storage: Postgres-only (same as the other admin API suites); all cases
// skip cleanly under memory/unreachable-server conditions.
//
// Quota note: the happy-path test-send writes a real `email_test_sent`
// audit row (that row IS the quota counter). The quota case derives its
// insert count from the CURRENT row count, so repeated runs stay correct;
// >10 suite runs against one persistent DB within 24h would legitimately
// trip the quota on the happy-path case (db-reset clears it).

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <fulla/drogon/utils/EmailService.h>

#include "HttpTestClient.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>

using fulla::test::http::loginAsAdmin;
using fulla::test::http::parseJsonBody;
using fulla::test::http::postgresAvailable;
using fulla::test::http::sendGet;
using fulla::test::http::sendPutJson;
using fulla::test::http::sendPostJson;
using fulla::test::http::serverReachable;
using fulla::test::http::statusIs;

#define ADMIN_EMAIL_TEMPLATE_SKIP_GUARD                       \
    do                                                        \
    {                                                         \
        if (!postgresAvailable() || !serverReachable())       \
        {                                                     \
            CHECK(true);                                      \
            return;                                           \
        }                                                     \
    } while (0)

namespace
{

// Spy implementation capturing both the plain-text and the rich (text+HTML)
// send paths. Installed via setEmailServiceForTesting BEFORE the request
// fires; the record happens at sendEmail entry, i.e. before the queued
// response reaches the test client (no race).
class SpyEmailService : public fulla::drogon::utils::IEmailService
{
  public:
    struct Sent
    {
        std::string to;
        std::string subject;
        std::string bodyText;
        std::string bodyHtml;  // empty on the plain-text path
    };

    void sendEmail(
      const std::string &to,
      const std::string &subject,
      const std::string &body,
      std::function<void(bool)> &&callback
    ) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Sent s;
            s.to = to;
            s.subject = subject;
            s.bodyText = body;
            sent_.push_back(std::move(s));
        }
        if (callback)
            callback(true);
    }

    void sendEmail(
      const std::string &to,
      const std::string &subject,
      const std::string &bodyText,
      const std::string &bodyHtml,
      std::function<void(bool)> &&callback
    ) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Sent s;
            s.to = to;
            s.subject = subject;
            s.bodyText = bodyText;
            s.bodyHtml = bodyHtml;
            sent_.push_back(std::move(s));
        }
        if (callback)
            callback(true);
    }

    std::deque<Sent> take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::move(sent_);
    }

  private:
    std::mutex mutex_;
    std::deque<Sent> sent_;
};

}  // namespace

// ---------------------------------------------------------------------------
// List: the V040 seeds are present (3 keys x 2 locales).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminEmailTemplate_List_ReturnsSeeds)
{
    ADMIN_EMAIL_TEMPLATE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/email-templates", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body["status"].asString() == "success");
    CHECK(body["templates"].isArray());
    CHECK(body["total"].asInt64() >= 6);
    bool hasVerificationEn = false;
    for (const auto &t : body["templates"])
    {
        if (t["template_key"].asString() == "email_verification" &&
            t["locale"].asString() == "en")
            hasVerificationEn = true;
    }
    CHECK(hasVerificationEn);
}

// ---------------------------------------------------------------------------
// Get one seeded template; get a missing (key, locale) -> 404 with the
// EMAIL_TEMPLATE_NOT_FOUND code.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminEmailTemplate_GetOne_AndNotFound)
{
    ADMIN_EMAIL_TEMPLATE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto resp = sendGet("/api/admin/email-templates/email_verification/en", *token);
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body["subject"].asString() == "Verify Your Email");
    CHECK(body["body_html"].asString().find("{{verify_link}}") != std::string::npos);

    auto missing = sendGet("/api/admin/email-templates/no_such_key/en", *token);
    REQUIRE(missing != nullptr);
    CHECK(statusIs(missing, drogon::k404NotFound));
    Json::Value errBody;
    REQUIRE(parseJsonBody(missing, errBody));
    CHECK(errBody["error"]["code"].asString() == "EMAIL_TEMPLATE_NOT_FOUND");
}

// ---------------------------------------------------------------------------
// Save-time content policy: script tag and out-of-allowlist link are
// rejected with EMAIL_TEMPLATE_CONTENT_REJECTED (400), and the stored row
// is NOT modified.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminEmailTemplate_Put_RejectsForbiddenContent)
{
    ADMIN_EMAIL_TEMPLATE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    for (const char *badHtml :
         {"<p>ok</p><script>alert(1)</script>",
          "<p><a href=\"https://evil.example.com/\">click</a></p>",
          "<p><a href=\"javascript:alert(1)\">x</a></p>"})
    {
        Json::Value payload;
        payload["subject"] = "Should be rejected";
        payload["body_html"] = badHtml;
        payload["body_text"] = "plain";
        auto resp = sendPutJson("/api/admin/email-templates/email_verification/en", payload, *token);
        REQUIRE(resp != nullptr);
        CHECK(statusIs(resp, drogon::k400BadRequest));
        Json::Value errBody;
        REQUIRE(parseJsonBody(resp, errBody));
        CHECK(errBody["error"]["code"].asString() == "EMAIL_TEMPLATE_CONTENT_REJECTED");
    }

    // The seed row must be untouched by the rejected writes.
    auto after = sendGet("/api/admin/email-templates/email_verification/en", *token);
    REQUIRE(after != nullptr);
    Json::Value afterBody;
    REQUIRE(parseJsonBody(after, afterBody));
    CHECK(afterBody["subject"].asString() == "Verify Your Email");
}

// ---------------------------------------------------------------------------
// Edit round-trip: PUT new content, GET it back, PUT the original back
// (persistent dev DB — leave the seeds as shipped).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminEmailTemplate_Put_EditRoundTrip)
{
    ADMIN_EMAIL_TEMPLATE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    const std::string path = "/api/admin/email-templates/password_reset/en";
    auto original = sendGet(path, *token);
    REQUIRE(original != nullptr);
    Json::Value originalBody;
    REQUIRE(parseJsonBody(original, originalBody));
    Json::Value restore;
    restore["subject"] = originalBody["subject"];
    restore["body_html"] = originalBody["body_html"];
    restore["body_text"] = originalBody["body_text"];

    Json::Value edited;
    edited["subject"] = "Edited reset subject";
    edited["body_html"] = "<p>Edited <a href=\"{{reset_link}}\">reset</a> body</p>";
    edited["body_text"] = "Edited plain body {{reset_link}}";
    auto put = sendPutJson(path, edited, *token);
    REQUIRE(put != nullptr);
    CHECK(statusIs(put, drogon::k200OK));

    auto got = sendGet(path, *token);
    REQUIRE(got != nullptr);
    Json::Value gotBody;
    REQUIRE(parseJsonBody(got, gotBody));
    CHECK(gotBody["subject"].asString() == "Edited reset subject");
    CHECK(gotBody["body_html"].asString().find("Edited") != std::string::npos);

    // Restore.
    auto restoreResp = sendPutJson(path, restore, *token);
    REQUIRE(restoreResp != nullptr);
    CHECK(statusIs(restoreResp, drogon::k200OK));
}

// ---------------------------------------------------------------------------
// No token -> 401 (the admin API family is Bearer-gated before RBAC even
// evaluates; a full non-admin-role 403 case needs a seeded regular user and
// is covered suite-wide by the AuthorizationFilter tests).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminEmailTemplate_NoToken_Unauthorized)
{
    ADMIN_EMAIL_TEMPLATE_SKIP_GUARD;

    // Plain user login (same recipe as loginAsAdmin but with user-scoped
    // grants is unavailable here; the admin-less path is covered by the
    // suite-wide convention: a missing/invalid token must be rejected too).
    auto resp = sendGet("/api/admin/email-templates");
    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k401Unauthorized));
}

// ---------------------------------------------------------------------------
// test-send happy path: edit a template, install the spy, test-send to the
// admin's own address, assert the spy captured the EDITED rendered content,
// restore the template.
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminEmailTemplate_TestSend_SpyCapturesEditedContent)
{
    ADMIN_EMAIL_TEMPLATE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    const std::string path = "/api/admin/email-templates/org_invitation/en";

    // Edit first (subject marker unique to this run).
    const std::string marker = "Invitation-subject-";
    auto original = sendGet(path, *token);
    REQUIRE(original != nullptr);
    Json::Value originalBody;
    REQUIRE(parseJsonBody(original, originalBody));
    Json::Value restore;
    restore["subject"] = originalBody["subject"];
    restore["body_html"] = originalBody["body_html"];
    restore["body_text"] = originalBody["body_text"];

    Json::Value edited;
    edited["subject"] = marker;
    edited["body_html"] =
      "<p>Invite to <strong>{{org_name}}</strong> token <code>{{invite_token}}</code></p>";
    edited["body_text"] = "Invite {{org_name}} {{invite_token}}";
    auto put = sendPutJson(path, edited, *token);
    REQUIRE(put != nullptr);
    CHECK(statusIs(put, drogon::k200OK));

    auto spy = std::make_shared<SpyEmailService>();
    fulla::drogon::utils::setEmailServiceForTesting(spy);

    Json::Value payload;
    payload["to"] = "admin@example.com";  // the seeded admin's own address
    auto resp = sendPostJson(path + "/test-send", payload, *token);
    // Always restore the factory before asserting anything below.
    fulla::drogon::utils::setEmailServiceForTesting(nullptr);

    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k200OK));
    Json::Value body;
    REQUIRE(parseJsonBody(resp, body));
    CHECK(body["subject"].asString() == marker);

    auto sent = spy->take();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].to == "admin@example.com");
    CHECK(sent[0].subject == marker);
    // Rich path carries both parts; synthetic vars are substituted (never
    // verbatim placeholders).
    CHECK(sent[0].bodyHtml.find("Example Organization") != std::string::npos);
    CHECK(sent[0].bodyHtml.find("{{org_name}}") == std::string::npos);
    CHECK(sent[0].bodyText.find("sample-invitation-token") != std::string::npos);

    // Restore the seed content.
    auto restoreResp = sendPutJson(path, restore, *token);
    REQUIRE(restoreResp != nullptr);
    CHECK(statusIs(restoreResp, drogon::k200OK));
}

// ---------------------------------------------------------------------------
// test-send whitelist: a foreign recipient is denied 403 with
// EMAIL_TEST_RECIPIENT_DENIED (no email leaves the process).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminEmailTemplate_TestSend_RecipientWhitelist)
{
    ADMIN_EMAIL_TEMPLATE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    auto spy = std::make_shared<SpyEmailService>();
    fulla::drogon::utils::setEmailServiceForTesting(spy);

    Json::Value payload;
    payload["to"] = "attacker@evil.example";
    auto resp = sendPostJson(
      "/api/admin/email-templates/email_verification/en/test-send", payload, *token);
    fulla::drogon::utils::setEmailServiceForTesting(nullptr);

    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k403Forbidden));
    Json::Value errBody;
    REQUIRE(parseJsonBody(resp, errBody));
    CHECK(errBody["error"]["code"].asString() == "EMAIL_TEST_RECIPIENT_DENIED");
    CHECK(spy->take().empty());
}

// ---------------------------------------------------------------------------
// Rolling-24h quota: pad the admin's email_test_sent audit count to the
// quota threshold with synthetic rows, assert 429 EMAIL_TEST_QUOTA_EXCEEDED,
// then remove the synthetic rows (later cases must not see them).
// ---------------------------------------------------------------------------
DROGON_TEST(Integration_P1_AdminEmailTemplate_TestSend_Rolling24hQuota)
{
    ADMIN_EMAIL_TEMPLATE_SKIP_GUARD;

    auto token = loginAsAdmin();
    REQUIRE(token.has_value());

    // Resolve the acting admin's public_sub (the actor_id the quota counts).
    auto db = drogon::app().getDbClient();
    REQUIRE(db != nullptr);
    auto subFuture =
      db->execSqlAsyncFuture("SELECT public_sub FROM users WHERE username = 'admin'");
    std::string adminSub;
    {
        auto rows = subFuture.get();
        REQUIRE(rows.size() == 1);
        adminSub = rows[0]["public_sub"].as<std::string>();
    }

    const int quota = [&] {
        auto config = drogon::app().getCustomConfig();
        if (config.isMember("email") && config["email"].isMember("test_send_quota_per_day") &&
            config["email"]["test_send_quota_per_day"].isInt())
            return config["email"]["test_send_quota_per_day"].asInt();
        return 10;
    }();

    // Current rolling-24h count decides how many synthetic rows to insert.
    auto countFuture = db->execSqlAsyncFuture(
      "SELECT count(*) AS n FROM audit_logs WHERE action = 'email_test_sent' AND actor_id = $1 "
      "AND timestamp > now() - interval '24 hours'",
      adminSub);
    int existing = 0;
    {
        auto rows = countFuture.get();
        existing = static_cast<int>(rows[0]["n"].as<int64_t>());
    }
    const int toInsert = quota - existing;
    const std::string marker = "p1-2-quota-pad-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    if (toInsert > 0)
    {
        for (int i = 0; i < toInsert; ++i)
        {
            auto f = db->execSqlAsyncFuture(
              "INSERT INTO audit_logs (actor_type, actor_id, action, target_type, target_id, "
              "outcome, timestamp) VALUES ('user', $1, 'email_test_sent', 'email_template', "
              "$2, 'success', now())",
              adminSub, marker);
            f.get();
        }
    }

    auto spy = std::make_shared<SpyEmailService>();
    fulla::drogon::utils::setEmailServiceForTesting(spy);
    Json::Value payload;
    payload["to"] = "admin@example.com";
    auto resp = sendPostJson(
      "/api/admin/email-templates/email_verification/en/test-send", payload, *token);
    fulla::drogon::utils::setEmailServiceForTesting(nullptr);

    // Cleanup BEFORE assertions so a failed CHECK still removes the padding.
    auto delFuture = db->execSqlAsyncFuture(
      "DELETE FROM audit_logs WHERE action = 'email_test_sent' AND target_id = $1", marker);
    delFuture.get();

    REQUIRE(resp != nullptr);
    CHECK(statusIs(resp, drogon::k429TooManyRequests));
    Json::Value errBody;
    REQUIRE(parseJsonBody(resp, errBody));
    CHECK(errBody["error"]["code"].asString() == "EMAIL_TEST_QUOTA_EXCEEDED");
    CHECK(spy->take().empty());
}
