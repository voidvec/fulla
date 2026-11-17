#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <fulla/identity/SocialLoginStateService.h>
#include <json/json.h>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace fulla::drogon::services
{

class SocialLoginGuard : public std::enable_shared_from_this<SocialLoginGuard>
{
  public:
    using ResponseCallback = std::function<void(const ::drogon::HttpResponsePtr &)>;
    using ValidationCallback = std::function<void(const std::string &, const std::string &)>;

    // P2-1: a registry-configured OIDC upstream (one upstream_providers row).
    // authorizeEndpoint is the FULLY-RESOLVED authorization endpoint base
    // (endpoint_overrides.authorize or the last successful discovery) -- the
    // runtime glue pre-resolves it so this synchronous layer never performs
    // I/O. Rows without a resolvable endpoint are skipped at refresh time.
    struct DynamicProvider
    {
        std::string key;
        std::string clientId;
        std::string redirectUri;
        std::string authorizeEndpoint;
        std::string scopes;
    };

    SocialLoginGuard(std::shared_ptr<identity::SocialLoginStateService> states,
                     Json::Value config, const std::vector<std::string> &providers,
                     bool allowInsecureLoopback);
    void authorize(const ::drogon::HttpRequestPtr &req, const std::string &provider,
                   ResponseCallback &&callback);
    void validate(const ::drogon::HttpRequestPtr &req, const std::string &provider,
                  ValidationCallback &&callback);
    // Replace the dynamic (registry) provider set; called by the runtime
    // glue after each registry refresh. Thread-safe.
    void setDynamicProviders(std::vector<DynamicProvider> providers);
    // Snapshot of the current dynamic set (login-page listing).
    std::vector<DynamicProvider> dynamicProviders() const;
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
    // Dynamic (registry) provider lookup; nullopt for config-static keys.
    std::optional<DynamicProvider> findDynamic(const std::string &provider) const;
    std::shared_ptr<identity::SocialLoginStateService> states_;
    Json::Value config_;
    std::map<std::string, std::pair<std::string, std::string>> providers_;
    std::set<std::string> origins_;
    bool secure_ = true;
    std::string cookieName_ = "__Host-fulla-social-binding";
    mutable std::mutex dynamicMutex_;
    std::vector<DynamicProvider> dynamicProviders_;
};

}  // namespace fulla::drogon::services
