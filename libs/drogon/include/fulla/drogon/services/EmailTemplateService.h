#pragma once

// P1-2 (#278): email template engine.
//
// Replaces the three hardcoded plain-text emails (verification /
// password reset / org invitation) with DB-backed, localized, editable
// templates (V040 email_templates; subject + HTML + text fallback),
// rendered through this service and delivered via the existing
// getEmailService() singleton (Console/SMTP).
//
// Security posture (deliverables/p1-fix-design-2026-10.md §P1-2, rev.1):
//   * variable values are HTML-escaped at substitution time (org names are
//     unfiltered free text -- OrganizationService createOrg);
//   * template bodies are statically validated at SAVE time (admin API):
//     no script/iframe/form/object/embed/base tags, no javascript:/data:/
//     vbscript: URLs, outbound http(s) hrefs only to configured
//     email.allowed_link_domains;
//   * link trust anchor (D8 red line): verification/reset link domains come
//     ONLY from global config (config.frontend.url) at the call sites --
//     org-controlled sources are never interpolated into security links.
//
// Fallback chain: DB (key, locale) -> DB (key, "en") -> compile-time
// built-in default (which also serves memory-mode deployments, where the
// engine never touches getDbClient() -- Debug-assert hazard, see
// MfaController's memory-mode guard for the same pattern).

#include <drogon/HttpRequest.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fulla::drogon::services
{

class EmailTemplateService
{
  public:
    struct RenderedEmail
    {
        std::string subject;
        std::string bodyHtml;
        std::string bodyText;
    };

    using RenderCallback = std::function<void(RenderedEmail)>;

    // The three production template keys (repo-wide grep confirms no fourth
    // production sender). Exposed for call sites, tests, and the admin API.
    static constexpr const char *kKeyEmailVerification = "email_verification";
    static constexpr const char *kKeyPasswordReset = "password_reset";
    static constexpr const char *kKeyOrgInvitation = "org_invitation";

    // ---- rendering ----

    /// Render `key` for `locale` with the full fallback chain and hand the
    /// result to `cb`. Never blocks; falls back to the built-in default on
    /// DB unavailability. Unknown keys surface as an empty RenderedEmail.
    static void render(
      const std::string &key,
      const std::string &locale,
      const std::map<std::string, std::string> &vars,
      RenderCallback &&cb
    );

    /// Facade for the three production call sites: render then send via
    /// getEmailService() (multipart/alternative under SMTP). Fire-and-forget
    /// -- same contract as the previous direct sendEmail calls.
    static void send(
      const std::string &key,
      const std::string &locale,
      const std::string &to,
      const std::map<std::string, std::string> &vars
    );

    // ---- pure helpers (unit-tested; no DB) ----

    /// {{var}} substitution. Substituted values are HTML-escaped when
    /// `escapeHtml` (HTML body); inserted verbatim for plain-text bodies.
    /// Unknown placeholders are left untouched (admin-visible typo signal).
    static std::string renderTemplateString(
      const std::string &tpl,
      const std::map<std::string, std::string> &vars,
      bool escapeHtml
    );

    /// Escape &, <, >, ", ' for HTML element/attribute contexts.
    static std::string htmlEscape(const std::string &s);

    /// Save-time static content policy. Returns "" when acceptable,
    /// otherwise a human-readable rejection reason (surfaced to the admin
    /// PUT as EMAIL_TEMPLATE_CONTENT_REJECTED). Reads the allowlist from
    /// custom config (email.allowed_link_domains).
    static std::string validateTemplateContent(
      const std::string &subject,
      const std::string &bodyHtml,
      const std::string &bodyText
    );

    /// Pure variant of the content policy (allowed domains injected — unit
    /// tests; the config-reading overload delegates here).
    static std::string validateTemplateContentWith(
      const std::string &subject,
      const std::string &bodyHtml,
      const std::string &bodyText,
      const std::vector<std::string> &allowedDomains
    );

    /// Locale resolution from the request's Accept-Language header: any
    /// "zh*" primary tag maps to the seeded "zh-CN", everything else to
    /// "en". Full q-value negotiation is out of scope (issue #278).
    static std::string resolveLocale(const ::drogon::HttpRequestPtr &req);

    /// Allowed outbound link domains from custom config
    /// (email.allowed_link_domains). Empty = no external http(s) hrefs
    /// allowed in saved templates.
    static std::vector<std::string> allowedLinkDomains();
};

}  // namespace fulla::drogon::services
