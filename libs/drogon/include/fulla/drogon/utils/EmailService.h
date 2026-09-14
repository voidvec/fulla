#pragma once

#include <string>
#include <functional>

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

  private:
    Config config_;
};

/**
 * @brief Get the global email service instance
 * Returns SmtpEmailService if configured, otherwise ConsoleEmailService
 */
IEmailService &getEmailService();

}  // namespace fulla::drogon::utils
