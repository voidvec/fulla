// tests/unit/services/EmailTemplateServiceTest.cc
//
// P1-2 (#278): unit tests for the email-template engine's pure helpers —
// {{var}} substitution with HTML escaping, the save-time content policy,
// and Accept-Language locale resolution. The DB fallback chain and the
// HTTP admin surface are covered by AdminEmailTemplateApiHttpTest
// (integration, Postgres-gated).

#include <drogon/drogon_test.h>
#include <drogon/drogon.h>
#include <fulla/drogon/services/EmailTemplateService.h>

#include <map>
#include <string>

using fulla::drogon::services::EmailTemplateService;

namespace
{
std::map<std::string, std::string> vars(std::initializer_list<std::pair<std::string, std::string>> init)
{
    std::map<std::string, std::string> m;
    for (const auto &p : init)
        m.emplace(p.first, p.second);
    return m;
}
}  // namespace

// ---------------------------------------------------------------------------
// htmlEscape
// ---------------------------------------------------------------------------

DROGON_TEST(Unit_P1_EmailTemplate_HtmlEscape_EscapesAllSpecials)
{
    CHECK(
      EmailTemplateService::htmlEscape(R"(<a href="x" class='y'>&</a>)") ==
      R"(&lt;a href=&quot;x&quot; class=&#39;y&#39;&gt;&amp;&lt;/a&gt;)");
}

// ---------------------------------------------------------------------------
// renderTemplateString
// ---------------------------------------------------------------------------

DROGON_TEST(Unit_P1_EmailTemplate_Render_SubstitutesAndEscapesInHtml)
{
    auto v = vars({{"org_name", R"("Acme" <& Friends>)"}});
    std::string out =
      EmailTemplateService::renderTemplateString("<p>{{org_name}}</p>", v, true);
    CHECK(out == "<p>&quot;Acme&quot; &lt;&amp; Friends&gt;</p>");
}

DROGON_TEST(Unit_P1_EmailTemplate_Render_PlainTextKeepsRawValues)
{
    auto v = vars({{"link", "https://example.com/verify?token=a&b=c"}});
    std::string out = EmailTemplateService::renderTemplateString("Click: {{link}}", v, false);
    CHECK(out == "Click: https://example.com/verify?token=a&b=c");
}

DROGON_TEST(Unit_P1_EmailTemplate_Render_UnknownPlaceholderKeptVerbatim)
{
    auto v = vars({{"known", "value"}});
    std::string out = EmailTemplateService::renderTemplateString("{{known}} {{typo}}", v, false);
    CHECK(out == "value {{typo}}");
}

DROGON_TEST(Unit_P1_EmailTemplate_Render_UnbalancedBracesLeftAlone)
{
    auto v = vars({{"a", "x"}});
    // An opener without a closing "{{" pair is literal text.
    CHECK(EmailTemplateService::renderTemplateString("{{a} } {{ ", v, false) == "{{a} } {{ ");
}

DROGON_TEST(Unit_P1_EmailTemplate_Render_NoVarsReturnsTemplate)
{
    std::string tpl = "plain body with {{nothing}} to fill";
    CHECK(EmailTemplateService::renderTemplateString(tpl, {}, true) == tpl);
}

// ---------------------------------------------------------------------------
// validateTemplateContentWith — tag / scheme blacklist
// ---------------------------------------------------------------------------

DROGON_TEST(Unit_P1_EmailTemplate_Policy_RejectsScriptTag)
{
    CHECK(!EmailTemplateService::validateTemplateContentWith(
              "Subject", "<p>hi</p><script>alert(1)</script>", "hi", {})
            .empty());
}

DROGON_TEST(Unit_P1_EmailTemplate_Policy_RejectsIframeFormObjectEmbedBase)
{
    for (const char *bad :
         {"<iframe src=\"https://x\"></iframe>",
          "<form action=\"/phish\"><input></form>",
          "<object data=\"https://x\"></object>",
          "<embed src=\"https://x\">",
          "<base href=\"https://evil.example/\">"})
    {
        CHECK(!EmailTemplateService::validateTemplateContentWith("Subject", bad, "hi", {}).empty());
    }
}

DROGON_TEST(Unit_P1_EmailTemplate_Policy_CaseInsensitiveTagDetection)
{
    CHECK(!EmailTemplateService::validateTemplateContentWith(
              "Subject", "<ScRiPt>alert(1)</sCrIpT>", "hi", {})
            .empty());
}

DROGON_TEST(Unit_P1_EmailTemplate_Policy_RejectsDangerousSchemes)
{
    for (const char *bad :
         {"<a href=\"javascript:alert(1)\">x</a>",
          "<a href=\"data:text/html;base64,x\">x</a>",
          "<a href=\"vbscript:msg\">x</a>"})
    {
        CHECK(!EmailTemplateService::validateTemplateContentWith("Subject", bad, "hi", {}).empty());
    }
    // Also scanned in subject and plain-text part (defense in depth).
    CHECK(!EmailTemplateService::validateTemplateContentWith(
              "hello <script>", "<p>ok</p>", "hi", {})
            .empty());
    CHECK(!EmailTemplateService::validateTemplateContentWith(
              "hello", "<p>ok</p>", "see javascript:void(0)", {})
            .empty());
}

// ---------------------------------------------------------------------------
// validateTemplateContentWith — outbound href allowlist
// ---------------------------------------------------------------------------

DROGON_TEST(Unit_P1_EmailTemplate_Policy_HrefDomainAllowlist)
{
    const std::vector<std::string> allow = {"portal.example.com", "docs.example.org"};

    // Allowed: exact host and subdomain, http or https.
    CHECK(EmailTemplateService::validateTemplateContentWith(
            "S", "<a href=\"https://portal.example.com/x\">x</a>", "t", allow)
            .empty());
    CHECK(EmailTemplateService::validateTemplateContentWith(
            "S", "<a href=\"http://www.portal.example.com/\">x</a>", "t", allow)
            .empty());
    // Variable placeholder links are engine-supplied — skipped.
    CHECK(EmailTemplateService::validateTemplateContentWith(
            "S", "<a href=\"{{verify_link}}\">x</a>", "t", allow)
            .empty());
    // Anchors / relative / mailto are not outbound.
    CHECK(EmailTemplateService::validateTemplateContentWith(
            "S", "<a href=\"#top\">x</a> <a href=\"/path\">x</a> <a href=\"mailto:a@b.c\">x</a>",
            "t", allow)
            .empty());

    // Rejected: foreign domain, unlisted domain, port smuggling, userinfo
    // smuggling, and any absolute link with an EMPTY allowlist.
    CHECK(!EmailTemplateService::validateTemplateContentWith(
              "S", "<a href=\"https://evil.example.com/\">x</a>", "t", allow)
            .empty());
    CHECK(!EmailTemplateService::validateTemplateContentWith(
              "S", "<a href=\"https://portal.example.com.evil.io/\">x</a>", "t", allow)
            .empty());
    CHECK(!EmailTemplateService::validateTemplateContentWith(
              "S", "<a href=\"https://evil.example@portal.example.com/\">x</a>", "t", allow)
            .empty());
    CHECK(!EmailTemplateService::validateTemplateContentWith(
              "S", "<a href=\"https://portal.example.com/\">x</a>", "t", {})
            .empty());
    // Unknown non-variable scheme (e.g. ftp) is rejected outright.
    CHECK(!EmailTemplateService::validateTemplateContentWith(
              "S", "<a href=\"ftp://portal.example.com/f\">x</a>", "t", allow)
            .empty());
}

DROGON_TEST(Unit_P1_EmailTemplate_Policy_HrefAttributeDetectionNotFooledByPrefix)
{
    // "xhref=" is not an href attribute — not treated as a link (and not a
    // bypass either: a real href= still follows in the same value).
    CHECK(EmailTemplateService::validateTemplateContentWith(
            "S", "<span data-xhref=\"https://evil.example.com/\">x</span>", "t", {})
            .empty());
    CHECK(!EmailTemplateService::validateTemplateContentWith(
              "S", "<a xhref=\"a\" href=\"https://evil.example.com/\">x</a>", "t", {})
            .empty());
    // Unquoted href values are still scanned.
    CHECK(!EmailTemplateService::validateTemplateContentWith(
              "S", "<a href=https://evil.example.com/>x</a>", "t", {})
            .empty());
}

// ---------------------------------------------------------------------------
// resolveLocale
// ---------------------------------------------------------------------------

DROGON_TEST(Unit_P1_EmailTemplate_ResolveLocale)
{
    auto makeReq = [](const char *acceptLanguage) {
        auto req = ::drogon::HttpRequest::newHttpRequest();
        if (acceptLanguage)
            req->addHeader("Accept-Language", acceptLanguage);
        return req;
    };
    CHECK(EmailTemplateService::resolveLocale(makeReq("zh-CN,zh;q=0.9,en;q=0.8")) == "zh-CN");
    CHECK(EmailTemplateService::resolveLocale(makeReq("zh")) == "zh-CN");
    CHECK(EmailTemplateService::resolveLocale(makeReq("zh-TW,en;q=0.5")) == "zh-CN");
    CHECK(EmailTemplateService::resolveLocale(makeReq("en-US,en;q=0.9")) == "en");
    CHECK(EmailTemplateService::resolveLocale(makeReq("fr-FR,fr;q=0.9")) == "en");
    CHECK(EmailTemplateService::resolveLocale(makeReq(nullptr)) == "en");
    CHECK(EmailTemplateService::resolveLocale(nullptr) == "en");
}
