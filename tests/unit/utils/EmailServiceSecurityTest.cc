// tests/unit/utils/EmailServiceSecurityTest.cc
//
// Security regression tests for the SMTP email path (PR #2 review, P1).
//
// Background: the previous SmtpEmailService built a `python3 -c "..."` program
// by string-concatenating the recipient / subject / body AND the SMTP
// credentials into a command run through popen() (a shell). That allowed:
//   (1) shell command injection via attacker-controlled email fields, and
//   (2) leaking SMTP credentials into the process argument list / logs.
//
// The fix sends mail via libcurl's native SMTP/SMTPS support (no shell, no
// concatenated command; credentials via CURLOPT_USERNAME/PASSWORD). The one
// remaining string-building step is the RFC 5322 message, which must NOT let a
// malicious header value (recipient/subject/from) inject extra headers or SMTP
// commands via CR/LF. SmtpEmailService::buildMimeMessage() sanitizes header
// fields for exactly this reason; these tests pin that behavior.
//
// _PR #2 P1 (EmailService.cc shell injection + credential leak)_

#include <drogon/drogon_test.h>
#include <fulla/drogon/utils/EmailService.h>

#include <string>

using fulla::drogon::utils::SmtpEmailService;

namespace
{
// Count occurrences of a substring (non-overlapping).
size_t countOccurrences(const std::string &haystack, const std::string &needle)
{
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos)
    {
        ++count;
        pos += needle.size();
    }
    return count;
}
}  // namespace

// A benign message has exactly the expected headers, all present once, and the
// header/body separator is a single blank line.
DROGON_TEST(Unit_P2_EmailSecurity_BuildMime_BenignMessage_WellFormed)
{
    std::string msg = SmtpEmailService::buildMimeMessage(
      "alice@example.com",
      "Verify your email",
      "Click the link to verify.",
      "OAuth2 Platform",
      "noreply@example.com"
    );

    CHECK(msg.find("To: <alice@example.com>") != std::string::npos);
    CHECK(msg.find("Subject: Verify your email") != std::string::npos);
    CHECK(msg.find("From: OAuth2 Platform <noreply@example.com>") != std::string::npos);
    // Header/body separator present, body after it verbatim.
    CHECK(msg.find("\r\n\r\nClick the link to verify.") != std::string::npos);
    // Exactly one Subject header.
    CHECK(countOccurrences(msg, "Subject:") == 1);
}

// CRLF injection in the recipient must NOT create extra headers. A classic
// attack: "victim@e.com\r\nBcc: attacker@e.com". The CR/LF must be stripped so
// the "Bcc:" text cannot start a new header line (it stays as inert inline text
// on the To: line, which is harmless — the security property is "no new header
// line", not "these bytes never appear").
DROGON_TEST(Unit_P2_EmailSecurity_BuildMime_CRLFInRecipient_NoHeaderInjection)
{
    std::string msg = SmtpEmailService::buildMimeMessage(
      "victim@example.com\r\nBcc: attacker@evil.com",
      "Subject",
      "Body",
      "Platform",
      "noreply@example.com"
    );

    // The injected "Bcc:" must NOT appear as a header line (i.e., right after a
    // CRLF). This is the actual header-injection check.
    CHECK(msg.find("\r\nBcc:") == std::string::npos);
    // No stray CRLF was introduced by the recipient value: the recipient is
    // flattened onto a single To: line.
    CHECK(countOccurrences(msg, "To: <") == 1);
}

// CRLF injection in the subject must not split into extra headers.
DROGON_TEST(Unit_P2_EmailSecurity_BuildMime_CRLFInSubject_NoHeaderInjection)
{
    std::string msg = SmtpEmailService::buildMimeMessage(
      "alice@example.com",
      "Hello\r\nContent-Type: text/html\r\nX-Injected: 1",
      "Body",
      "Platform",
      "noreply@example.com"
    );

    // Neither injected field may appear as a NEW header line (after a CRLF).
    CHECK(msg.find("\r\nX-Injected:") == std::string::npos);
    CHECK(msg.find("\r\nContent-Type: text/html") == std::string::npos);
    // Exactly one Content-Type HEADER line — the legitimate text/plain one,
    // emitted by buildMimeMessage after a CRLF.
    CHECK(countOccurrences(msg, "\r\nContent-Type: text/plain; charset=UTF-8") == 1);
}

// Shell metacharacters in the recipient are now just inert text — there is no
// shell anymore. They must not break message structure (no injected headers).
// This is the regression test for the original command-injection vector.
DROGON_TEST(Unit_P2_EmailSecurity_BuildMime_ShellMetacharsInRecipient_Inert)
{
    const std::string maliciousTo = "x'; touch /tmp/pwned; '@example.com";
    std::string msg = SmtpEmailService::buildMimeMessage(
      maliciousTo, "Subject", "Body", "Platform", "noreply@example.com"
    );

    // The value is carried as inert text on a single To: line (no CRLF in it),
    // and crucially there is no shell command construction anywhere — the
    // payload cannot be executed. Structure is intact: one To:, one Subject:.
    CHECK(countOccurrences(msg, "To: <") == 1);
    CHECK(countOccurrences(msg, "Subject:") == 1);
    // No stray newline introduced by the input.
    CHECK(msg.find("touch /tmp/pwned\r\n") == std::string::npos);
}

// Bare LF (not just CRLF) must also be stripped from header fields so it cannot
// start a new header line.
DROGON_TEST(Unit_P2_EmailSecurity_BuildMime_BareLFInFrom_Sanitized)
{
    std::string msg = SmtpEmailService::buildMimeMessage(
      "alice@example.com", "Subject", "Body", "Platform\nX-Evil: yes", "noreply@example.com"
    );

    // The injected field must not appear as a NEW header line (after CR/LF);
    // the bare LF is stripped so "X-Evil:" stays inert on the From: line.
    CHECK(msg.find("\nX-Evil:") == std::string::npos);
    CHECK(countOccurrences(msg, "From:") == 1);
}

// The body MAY legitimately contain newlines; they must be preserved (only
// HEADER fields are sanitized, not the body).
DROGON_TEST(Unit_P2_EmailSecurity_BuildMime_BodyNewlinesPreserved)
{
    std::string msg = SmtpEmailService::buildMimeMessage(
      "alice@example.com", "Subject", "Line1\r\nLine2\r\nLine3", "Platform", "noreply@example.com"
    );

    CHECK(msg.find("Line1\r\nLine2\r\nLine3") != std::string::npos);
}

// ---------------------------------------------------------------------------
// P1-2 (#278): multipart/alternative assembly (buildMimeMessageMultipart).
// Same sanitization contract as the single-part builder, plus structural
// requirements of RFC 2046 §5.1.4 (boundary-delimited parts, text part
// first, HTML part last/preferred).
// ---------------------------------------------------------------------------

DROGON_TEST(Unit_P1_EmailSecurity_BuildMimeMultipart_Structure)
{
    std::string msg = SmtpEmailService::buildMimeMessageMultipart(
      "alice@example.com",
      "Verify your email",
      "Click the link to verify.",
      "<p>Click <a href=\"https://example.com/v\">the link</a>.</p>",
      "OAuth2 Platform",
      "noreply@example.com"
    );

    const std::string boundary = SmtpEmailService::multipartBoundary();
    // One multipart Content-Type header naming the boundary.
    CHECK(msg.find("Content-Type: multipart/alternative; boundary=\"" + boundary + "\"") !=
          std::string::npos);
    // Opening boundary appears twice (one per part); the closing delimiter
    // "--boundary--" once.
    CHECK(countOccurrences(msg, "--" + boundary + "\r\n") == 2);
    CHECK(countOccurrences(msg, "--" + boundary + "--\r\n") == 1);
    // Both part content types, text before HTML.
    std::size_t textPart = msg.find("Content-Type: text/plain; charset=UTF-8");
    std::size_t htmlPart = msg.find("Content-Type: text/html; charset=UTF-8");
    REQUIRE(textPart != std::string::npos);
    REQUIRE(htmlPart != std::string::npos);
    CHECK(textPart < htmlPart);
    // Both bodies verbatim inside their parts.
    CHECK(msg.find("Click the link to verify.") != std::string::npos);
    CHECK(msg.find(R"(<p>Click <a href="https://example.com/v">the link</a>.</p>)") !=
          std::string::npos);
}

DROGON_TEST(Unit_P1_EmailSecurity_BuildMimeMultipart_HeaderInjectionInert)
{
    std::string msg = SmtpEmailService::buildMimeMessageMultipart(
      "alice@example.com",
      "Subject\r\nBcc: victim@example.com",
      "text body",
      "<p>html body</p>",
      "Platform\r\nX-Evil: yes",
      "noreply@example.com"
    );

    // CR/LF stripped from header values: no injected Bcc/X-Evil header line.
    CHECK(msg.find("\r\nBcc:") == std::string::npos);
    CHECK(msg.find("\nX-Evil:") == std::string::npos);
    CHECK(countOccurrences(msg, "Subject:") == 1);
    CHECK(countOccurrences(msg, "From:") == 1);
}
