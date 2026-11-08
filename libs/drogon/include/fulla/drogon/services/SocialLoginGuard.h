#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <fulla/identity/SocialLoginStateService.h>
#include <json/json.h>
#include <map>
#include <set>
#include <vector>

namespace fulla::drogon::services
{

class SocialLoginGuard : public std::enable_shared_from_this<SocialLoginGuard>
{
  public:
    using ResponseCallback = std::function<void(const ::drogon::HttpResponsePtr &)>;
    using ValidationCallback = std::function<void(const std::string &, const std::string &)>;
    SocialLoginGuard(std::shared_ptr<identity::SocialLoginStateService> states,
                     Json::Value config, const std::vector<std::string> &providers,
                     bool allowInsecureLoopback);
    void authorize(const ::drogon::HttpRequestPtr &req, const std::string &provider,
                   ResponseCallback &&callback);
    void validate(const ::drogon::HttpRequestPtr &req, const std::string &provider,
                  ValidationCallback &&callback);
    static ResponseCallback noStore(ResponseCallback &&callback);
    static void error(const ::drogon::HttpRequestPtr &req, ResponseCallback &&callback,
                      const std::string &code);

  private:
    std::string requestError(const ::drogon::HttpRequestPtr &req,
                             const std::string &provider) const;
    identity::SocialLoginContext context(const std::string &provider,
                                         const std::string &binding) const;
    void setBinding(const ::drogon::HttpResponsePtr &response,
                    const std::string &binding, int maxAge = 600) const;
    std::shared_ptr<identity::SocialLoginStateService> states_;
    Json::Value config_;
    std::map<std::string, std::pair<std::string, std::string>> providers_;
    std::set<std::string> origins_;
    bool secure_ = true;
    std::string cookieName_ = "__Host-fulla-social-binding";
};

}  // namespace fulla::drogon::services
