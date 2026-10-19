#pragma once

#include <string>
#include <functional>
#include <memory>

namespace fulla::drogon::utils
{

/**
 * @brief Abstract email service interface
 */
class IEmailService
{
  public:
    virtual ~IEmailService() = default;

    virtual void sendEmail(
      const std::string &to,
      const std::string &subject,
      const std::string &body,
      std::function<void(bool)> &&callback
    ) = 0;

    /**
     * @brief Rich send (P1-2 email templates): plain-text + HTML body.
     *
     * Implementations that can carry both parts (SmtpEmailService:
     * multipart/alternative) do so; the default implementation drops the
     * HTML part and delegates to the plain-text send, which keeps Console
     * mode and any other implementation source-compatible.
     */
    virtual void sendEmail(
      const std::string &to,
      const std::string &subject,
      const std::string &bodyText,
      const std::string &bodyHtml,
      std::function<void(bool)> &&callback
    )
    {
        sendEmail(to, subject, bodyText, std::move(callback));
    }
};

/**
 * @brief Console email service for development/testing
 * Logs emails to console instead of sending them
 */
class ConsoleEmailService : public IEmailService
{
  public:
    void sendEmail(
      const std::string &to,
      const std::string &subject,
      const std::string &body,
      std::function<void(bool)> &&callback
    ) override;
};

/**
 * @brief SMTP email service for production
 * Sends real emails via SMTP (163, Gmail, SendGrid, etc.)
 */
class SmtpEmailService : public IEmailService
{
  public:
    struct Config
    {
        std::string host = "smtp.163.com";
        int port = 465;
        std::string username;  // full email: xxx@163.com
        std::string password;  // authorization code
        std::string fromName = "Fulla";
        bool useSsl = true;
    };

    explicit SmtpEmailService(const Config &config) : config_(config)
    {
    }

    void sendEmail(
      const std::string &to,
      const std::string &subject,
      const std::string &body,
      std::function<void(bool)> &&callback
    ) override;

    // P1-2: text + HTML via multipart/alternative. An empty bodyHtml sends
    // the legacy single-part text/plain message (byte-compatible with the
    // pre-template path).
    void sendEmail(
      const std::string &to,
      const std::string &subject,
      const std::string &bodyText,
      const std::string &bodyHtml,
      std::function<void(bool)> &&callback
    ) override;

    /**
     * @brief Build the RFC 5322 message for an email.
     *
     * Header fields (to/subject/from) are sanitized so CR/LF cannot inject
     * additional headers or SMTP commands; the body is appended verbatim after
     * the header/body separator. Exposed as a static method so security
     * regression tests can assert that no header/command injection is possible
     * regardless of input (defense for PR #2 P1 finding).
     */
    static std::string buildMimeMessage(
      const std::string &to,
      const std::string &subject,
      const std::string &body,
      const std::string &fromName,
      const std::string &fromAddress
    );

    /**
     * @brief Build a multipart/alternative RFC 5322 message (P1-2).
     *
     * Part 1 is the plain-text body, part 2 the HTML body (RFC 2046
     * §5.1.4: last part is the richest, preferred format). Same header
     * sanitization contract as buildMimeMessage. The boundary is a fixed
     * constant (deterministic output for security tests); template bodies
     * are admin-authored and reject-validated at save time, and none of the
     * seeded templates contain the marker.
     */
    static std::string buildMimeMessageMultipart(
      const std::string &to,
      const std::string &subject,
      const std::string &bodyText,
      const std::string &bodyHtml,
      const std::string &fromName,
      const std::string &fromAddress
    );

    /// The multipart boundary used by buildMimeMessageMultipart (tests).
    static const char *multipartBoundary();

  private:
    Config config_;
};

/**
 * @brief Get the global email service instance
 * Returns SmtpEmailService if configured, otherwise ConsoleEmailService
 */
IEmailService &getEmailService();

/**
 * @brief Test seam (P1-2): override the process-wide email service.
 *
 * getEmailService() is a once_flag singleton factory with no HTTP-level
 * injection point; integration tests install a spy here to capture sent
 * messages. Call with nullptr (or let it go out of scope via reset) before
 * teardown. Not thread-safe with concurrent senders -- install BEFORE
 * issuing requests.
 */
void setEmailServiceForTesting(std::shared_ptr<IEmailService> overrideService);

}  // namespace fulla::drogon::utils
