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
    // P2-1c: hot-update a CONFIG-STATIC provider from its registry row
    // (the runtime applies this on refresh; admin edits take effect without
    // a restart). enabled=false removes the key entirely (authorize and
    // validate answer 404 exactly like a never-configured provider); an
    // enabled row for a key config never configured ENABLES it (redirect
    // follows the shared per-key callback convention). The client_id
    // participates in the state binding, so changing it un-binds any
    // in-flight transactions issued under the old credential.
    void updateStaticProvider(
      const std::string &provider, const std::string &clientId,
      const std::string &clientSecret, bool enabled);
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
    // P2-1c: runtime-mutable (registry rows drive the three legacy social
    // keys); guard every access with staticMutex_.
    struct StaticEntry
    {
        std::string clientId;
        std::string clientSecret;
        std::string redirectUri;
    };
    std::map<std::string, StaticEntry> providers_;
    std::set<std::string> origins_;
    bool secure_ = true;
    std::string cookieName_ = "__Host-fulla-social-binding";
    mutable std::mutex dynamicMutex_;
    std::vector<DynamicProvider> dynamicProviders_;
    mutable std::mutex staticMutex_;
};

}  // namespace fulla::drogon::services
