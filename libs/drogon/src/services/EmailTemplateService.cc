#include <fulla/drogon/services/EmailTemplateService.h>

#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <fulla/drogon/utils/EmailService.h>
#include <fulla/storage/postgres/models/EmailTemplates.h>

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <cctype>

namespace fulla::drogon::services
{

namespace
{
using namespace ::drogon::orm;
using TemplateModel = ::drogon_model::fulla_db::EmailTemplates;

// ---------------------------------------------------------------------------
// Built-in defaults (compile-time copy of the V040 seed). The last rung of
// the fallback chain and the ONLY data source in memory mode.
// ---------------------------------------------------------------------------

EmailTemplateService::RenderedEmail builtInVerification(const std::string &locale)
{
    EmailTemplateService::RenderedEmail e;
    if (locale == "zh-CN")
    {
        e.subject = "验证您的邮箱";
        e.bodyHtml =
          "<p>请点击下方链接验证您的邮箱：</p>"
          "<p><a href=\"{{verify_link}}\">验证邮箱</a></p>"
          "<p>链接有效期 24 小时。如果您没有注册账号，请忽略本邮件。</p>";
        e.bodyText = "请点击以下链接验证您的邮箱：\n\n{{verify_link}}\n\n本链接有效期 24 小时。";
    }
    else
    {
        e.subject = "Verify Your Email";
        e.bodyHtml =
          "<p>Please verify your email by clicking the link below:</p>"
          "<p><a href=\"{{verify_link}}\">Verify your email</a></p>"
          "<p>This link expires in 24 hours. If you did not create an account, you can "
          "ignore this email.</p>";
        e.bodyText =
          "Please verify your email by clicking:\n\n{{verify_link}}\n\nThis link expires in "
          "24 hours.";
    }
    return e;
}

EmailTemplateService::RenderedEmail builtInReset(const std::string &locale)
{
    EmailTemplateService::RenderedEmail e;
    if (locale == "zh-CN")
    {
        e.subject = "重置密码请求";
        e.bodyHtml =
          "<p>我们收到了重置您密码的请求。</p>"
          "<p><a href=\"{{reset_link}}\">重置密码</a></p>"
          "<p>链接有效期 15 分钟。如果这不是您本人的操作，请忽略本邮件。</p>";
        e.bodyText = "请点击以下链接重置您的密码：\n\n{{reset_link}}\n\n本链接有效期 15 分钟。";
    }
    else
    {
        e.subject = "Password Reset Request";
        e.bodyHtml =
          "<p>We received a request to reset your password.</p>"
          "<p><a href=\"{{reset_link}}\">Reset your password</a></p>"
          "<p>This link expires in 15 minutes. If you did not request a reset, you can "
          "ignore this email.</p>";
        e.bodyText =
          "Click the following link to reset your password:\n\n{{reset_link}}\n\nThis link "
          "expires in 15 minutes.";
    }
    return e;
}

EmailTemplateService::RenderedEmail builtInInvitation(const std::string &locale)
{
    EmailTemplateService::RenderedEmail e;
    if (locale == "zh-CN")
    {
        e.subject = "Fulla 组织邀请";
        e.bodyHtml =
          "<p>您被邀请加入 Fulla 上的组织 <strong>{{org_name}}</strong>。</p>"
          "<p>邀请令牌（有效期 72 小时，仅可使用一次）：</p>"
          "<p><code>{{invite_token}}</code></p>"
          "<p>请登录门户，打开“我的组织”，在“接受邀请”处粘贴该令牌。如果您没有预期收到此邀请，"
          "请忽略本邮件。</p>";
        e.bodyText =
          "您被邀请加入 Fulla 上的组织“{{org_name}}”。\n\n"
          "邀请令牌（有效期 72 小时，仅可使用一次）：\n  {{invite_token}}\n\n"
          "请登录门户，打开“我的组织”，在“接受邀请”处粘贴该令牌。如果您没有预期收到此邀请，"
          "请忽略本邮件。";
    }
    else
    {
        e.subject = "Fulla Organization Invitation";
        e.bodyHtml =
          "<p>You have been invited to join the organization <strong>{{org_name}}</strong> on "
          "Fulla.</p>"
          "<p>Invitation token (valid for 72 hours, single use):</p>"
          "<p><code>{{invite_token}}</code></p>"
          "<p>Sign in to the portal, open My Organizations, and paste the token under "
          "\"Accept an invitation\". If you did not expect this invitation you can ignore "
          "this email.</p>";
        e.bodyText =
          "You have been invited to join the organization \"{{org_name}}\" on Fulla.\n\n"
          "Invitation token (valid for 72 hours, single use):\n  {{invite_token}}\n\n"
          "Sign in to the portal, open My Organizations, and paste the token under \"Accept "
          "an invitation\". If you did not expect this invitation you can ignore this email.";
    }
    return e;
}

bool containsCaseInsensitive(const std::string &haystack, const std::string &needle)
{
    if (needle.size() > haystack.size())
        return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        std::size_t j = 0;
        while (j < needle.size() &&
               std::tolower(static_cast<unsigned char>(haystack[i + j])) ==
                 std::tolower(static_cast<unsigned char>(needle[j])))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}

// Host of an absolute http(s) URL: text after the scheme authority marker up
// to the first '/', '?' or '#'. Lowercased (host comparison is case-
// insensitive). Returns "" for URLs with no authority OR with userinfo
// ("user@host") — userinfo is never legitimate in a template link and must
// not be normalized away (it would smuggle a foreign host past the check).
std::string urlHost(const std::string &url)
{
    std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
        return "";
    std::size_t hostStart = schemeEnd + 3;
    std::size_t hostEnd = url.find_first_of("/?#", hostStart);
    std::string host =
      url.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
    if (host.find('@') != std::string::npos)
        return "";
    // Strip port.
    std::size_t colon = host.find(':');
    if (colon != std::string::npos)
        host = host.substr(0, colon);
    std::string lower;
    lower.reserve(host.size());
    for (char c : host)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

std::string EmailTemplateService::htmlEscape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

std::string EmailTemplateService::renderTemplateString(
  const std::string &tpl,
  const std::map<std::string, std::string> &vars,
  bool escapeHtml
)
{
    std::string out;
    out.reserve(tpl.size());
    std::size_t i = 0;
    while (i < tpl.size())
    {
        std::size_t open = tpl.find("{{", i);
        if (open == std::string::npos)
        {
            out.append(tpl, i, std::string::npos);
            break;
        }
        std::size_t close = tpl.find("}}", open + 2);
        if (close == std::string::npos)
        {
            out.append(tpl, i, std::string::npos);
            break;
        }
        out.append(tpl, i, open - i);
        std::string name = tpl.substr(open + 2, close - open - 2);
        auto it = vars.find(name);
        if (it != vars.end())
            out += escapeHtml ? htmlEscape(it->second) : it->second;
        else
            out.append(tpl, open, close + 2 - open);  // unknown var: keep verbatim
        i = close + 2;
    }
    return out;
}

std::vector<std::string> EmailTemplateService::allowedLinkDomains()
{
    std::vector<std::string> domains;
    auto config = ::drogon::app().getCustomConfig();
    if (config.isMember("email") && config["email"].isMember("allowed_link_domains") &&
        config["email"]["allowed_link_domains"].isArray())
    {
        for (const auto &d : config["email"]["allowed_link_domains"])
        {
            if (d.isString())
            {
                std::string s = d.asString();
                // Normalize: lowercase, strip scheme and trailing dot/slash
                // so the config accepts both "example.com" and
                // "https://example.com/" spellings.
                std::size_t scheme = s.find("://");
                if (scheme != std::string::npos)
                    s = s.substr(scheme + 3);
                while (!s.empty() && (s.back() == '/' || s.back() == '.'))
                    s.pop_back();
                std::string lower;
                for (char c : s)
                    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                if (!lower.empty())
                    domains.push_back(lower);
            }
        }
    }
    return domains;
}

std::string EmailTemplateService::validateTemplateContent(
  const std::string &subject,
  const std::string &bodyHtml,
  const std::string &bodyText
)
{
    return validateTemplateContentWith(subject, bodyHtml, bodyText, allowedLinkDomains());
}

std::string EmailTemplateService::validateTemplateContentWith(
  const std::string &subject,
  const std::string &bodyHtml,
  const std::string &bodyText,
  const std::vector<std::string> &allowedDomains
)
{
    // Tag blacklist (save-time): anything that can execute or capture input.
    static const char *kForbiddenTags[] = {
      "<script", "<iframe", "<form", "<object", "<embed", "<base",
    };
    // Dangerous URL schemes anywhere in any part (attribute or not).
    static const char *kForbiddenSchemes[] = {"javascript:", "vbscript:", "data:"};

    auto scan = [&](const std::string &part, const char *what) -> std::string {
        for (const char *tag : kForbiddenTags)
            if (containsCaseInsensitive(part, tag))
                return std::string("forbidden tag '") + tag + "' in " + what;
        for (const char *scheme : kForbiddenSchemes)
            if (containsCaseInsensitive(part, scheme))
                return std::string("forbidden URL scheme '") + scheme + "' in " + what;
        return "";
    };

    if (std::string err = scan(subject, "subject"); !err.empty())
        return err;
    if (std::string err = scan(bodyText, "body_text"); !err.empty())
        return err;
    if (std::string err = scan(bodyHtml, "body_html"); !err.empty())
        return err;

    // Outbound href policy in the HTML body: absolute http(s) links must
    // resolve to a configured allowed domain. Variable placeholders
    // ({{verify_link}} etc.) are skipped -- their values are engine-supplied
    // and HTML-escaped at substitution time.
    const std::vector<std::string> &allowed = allowedDomains;
    const std::string &html = bodyHtml;
    std::size_t pos = 0;
    while (true)
    {
        std::size_t href = html.find("href", pos);
        if (href == std::string::npos)
            break;
        // Skip matches that are not attribute names (e.g. "xhref").
        bool leftBounded = href == 0 || !std::isalpha(static_cast<unsigned char>(html[href - 1]));
        std::size_t after = href + 4;
        while (after < html.size() && std::isspace(static_cast<unsigned char>(html[after])))
            ++after;
        if (!leftBounded || after >= html.size() || (html[after] != '=' && html[after] != ':'))
        {
            pos = href + 4;
            continue;
        }
        ++after;  // consume '=' (or ':')
        while (after < html.size() && std::isspace(static_cast<unsigned char>(html[after])))
            ++after;
        char quote = 0;
        if (after < html.size() && (html[after] == '"' || html[after] == '\''))
        {
            quote = html[after];
            ++after;
        }
        std::size_t valueStart = after;
        std::size_t valueEnd;
        if (quote != 0)
        {
            valueEnd = html.find(quote, valueStart);
            if (valueEnd == std::string::npos)
                valueEnd = html.size();
        }
        else
        {
            valueEnd = html.find_first_of(" \t\r\n>", valueStart);
            if (valueEnd == std::string::npos)
                valueEnd = html.size();
        }
        std::string value = html.substr(valueStart, valueEnd - valueStart);
        pos = valueEnd;

        if (value.find("{{") != std::string::npos)
            continue;  // engine-supplied variable link
        if (value.empty() || value[0] == '#' || value[0] == '/' || value.rfind("mailto:", 0) == 0)
            continue;  // anchor / relative / mailto

        std::string lowerValue;
        lowerValue.reserve(value.size());
        for (char c : value)
            lowerValue.push_back(
              static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (lowerValue.rfind("http://", 0) == 0 || lowerValue.rfind("https://", 0) == 0)
        {
            std::string host = urlHost(lowerValue);
            bool ok = !host.empty();
            if (ok)
            {
                bool matched = false;
                for (const auto &d : allowed)
                {
                    if (host == d || (host.size() > d.size() &&
                                      host.compare(host.size() - d.size() - 1, d.size() + 1, "." + d) == 0))
                    {
                        matched = true;
                        break;
                    }
                }
                ok = matched;
            }
            if (!ok)
                return "outbound link '" + value + "' is not in email.allowed_link_domains";
        }
        else
        {
            return "unsupported href scheme in link '" + value + "'";
        }
    }
    return "";
}

std::string EmailTemplateService::resolveLocale(const ::drogon::HttpRequestPtr &req)
{
    if (!req)
        return "en";
    std::string header = req->getHeader("Accept-Language");
    if (header.empty())
        return "en";
    std::string primary = header.substr(0, header.find(','));
    // Strip q-suffixes ("zh;q=0.9") and whitespace.
    std::size_t semi = primary.find(';');
    if (semi != std::string::npos)
        primary = primary.substr(0, semi);
    std::string trimmed;
    for (char c : primary)
        if (!std::isspace(static_cast<unsigned char>(c)))
            trimmed.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return trimmed.rfind("zh", 0) == 0 ? "zh-CN" : "en";
}

// ---------------------------------------------------------------------------
// Rendering / sending
// ---------------------------------------------------------------------------

namespace
{
EmailTemplateService::RenderedEmail builtInDefault(
  const std::string &key,
  const std::string &locale
)
{
    if (key == EmailTemplateService::kKeyEmailVerification)
        return builtInVerification(locale);
    if (key == EmailTemplateService::kKeyPasswordReset)
        return builtInReset(locale);
    if (key == EmailTemplateService::kKeyOrgInvitation)
        return builtInInvitation(locale);
    return {};
}

EmailTemplateService::RenderedEmail renderRow(
  const TemplateModel &row,
  const std::map<std::string, std::string> &vars
)
{
    EmailTemplateService::RenderedEmail e;
    e.subject = EmailTemplateService::renderTemplateString(row.getValueOfSubject(), vars, false);
    e.bodyHtml = EmailTemplateService::renderTemplateString(row.getValueOfBodyHtml(), vars, true);
    e.bodyText =
      EmailTemplateService::renderTemplateString(row.getValueOfBodyText(), vars, false);
    return e;
}

// Apply variable substitution to an already-extracted template (the built-in
// defaults are raw templates, same as DB rows).
EmailTemplateService::RenderedEmail renderBuiltIn(
  const EmailTemplateService::RenderedEmail &tpl,
  const std::map<std::string, std::string> &vars
)
{
    EmailTemplateService::RenderedEmail e;
    e.subject = EmailTemplateService::renderTemplateString(tpl.subject, vars, false);
    e.bodyHtml = EmailTemplateService::renderTemplateString(tpl.bodyHtml, vars, true);
    e.bodyText = EmailTemplateService::renderTemplateString(tpl.bodyText, vars, false);
    return e;
}
}  // namespace

void EmailTemplateService::render(
  const std::string &key,
  const std::string &locale,
  const std::map<std::string, std::string> &vars,
  RenderCallback &&cb
)
{
    auto finish = std::make_shared<RenderCallback>(std::move(cb));

    // Memory mode: the engine must never reach getDbClient() (Debug builds
    // ABORT when no db_clients are configured). The built-in default is the
    // whole data source.
    auto *plugin = ::drogon::app().getPlugin<::OAuth2Plugin>();
    if (plugin && plugin->getStorageType() == "memory")
    {
        (*finish)(renderBuiltIn(builtInDefault(key, locale), vars));
        return;
    }

    ::drogon::orm::DbClientPtr db;
    try
    {
        db = ::drogon::app().getDbClient();
    }
    catch (...)
    {
        (*finish)(renderBuiltIn(builtInDefault(key, locale), vars));
        return;
    }
    if (!db)
    {
        (*finish)(renderBuiltIn(builtInDefault(key, locale), vars));
        return;
    }

    // Fallback chain, innermost first. Each rung is a self-contained async
    // step; shared_ptr copies keep every lambda's capture list alive and
    // each Mapper construction gets its own try/catch (db-operations.md).
    auto useBuiltIn = std::make_shared<std::function<void()>>(
      [finish, key, locale, vars]() { (*finish)(renderBuiltIn(builtInDefault(key, locale), vars)); });

    auto withEnRow = std::make_shared<std::function<void(const TemplateModel &)>>(
      [finish, vars](const TemplateModel &row) { (*finish)(renderRow(row, vars)); });

    auto tryEn = std::make_shared<std::function<void()>>(
      [db, key, locale, vars, finish, useBuiltIn, withEnRow]() {
          if (locale == "en")
          {
              (*useBuiltIn)();
              return;
          }
          try
          {
              Mapper<TemplateModel> mapper(db);
              mapper.findBy(
                Criteria(TemplateModel::Cols::_template_key, CompareOperator::EQ, key) &&
                  Criteria(TemplateModel::Cols::_locale, CompareOperator::EQ, std::string("en")),
                [finish, vars, withEnRow, useBuiltIn](const std::vector<TemplateModel> &rows) {
                    if (!rows.empty())
                        (*withEnRow)(rows.front());
                    else
                        (*useBuiltIn)();
                },
                [useBuiltIn](const DrogonDbException &) { (*useBuiltIn)(); });
          }
          catch (...)
          {
              (*useBuiltIn)();
          }
      });

    try
    {
        Mapper<TemplateModel> mapper(db);
        mapper.findBy(
          Criteria(TemplateModel::Cols::_template_key, CompareOperator::EQ, key) &&
            Criteria(TemplateModel::Cols::_locale, CompareOperator::EQ, locale),
          [finish, vars, tryEn](const std::vector<TemplateModel> &rows) {
              if (!rows.empty())
                  (*finish)(renderRow(rows.front(), vars));
              else
                  (*tryEn)();  // requested-locale miss -> (key, "en")
          },
          [tryEn](const DrogonDbException &) { (*tryEn)(); });
    }
    catch (...)
    {
        (*tryEn)();
    }
}

void EmailTemplateService::send(
  const std::string &key,
  const std::string &locale,
  const std::string &to,
  const std::map<std::string, std::string> &vars
)
{
    render(
      key,
      locale,
      vars,
      [key, to](RenderedEmail email) {
          if (email.subject.empty())
          {
              LOG_WARN << "EmailTemplateService: refusing to send template '" << key
                       << "' with empty subject (unknown key?)";
              return;
          }
          ::fulla::drogon::utils::getEmailService().sendEmail(
            to,
            email.subject,
            email.bodyText,
            email.bodyHtml,
            [](bool ok) {
                if (!ok)
                    LOG_WARN << "EmailTemplateService: delivery failed";
            });
      });
}

}  // namespace fulla::drogon::services
