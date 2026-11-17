// IUpstreamProviderRepository.h
//
// P2-1: port for the OIDC-in upstream provider registry (the
// `upstream_providers` table). One row = one externally-issued OIDC identity
// source this deployment can offer on the login page; rows are administered
// at runtime through /api/admin/upstream-providers and consumed by the
// generic federation flow (authorize -> code -> token -> userinfo) which
// links accounts through oauth2_subject_mappings.provider = provider_key --
// the same column and lifecycle the google/wechat/github links use.
//
// Convention: async-callback port in the style of
// ISocialAccountRepository.h (callback-style, no futures, no coroutines);
// implementations must deliver exactly one callback per call, including on
// failure (see .claude/rules/db-operations.md).

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fulla::identity
{

// Plain-value mirror of one `upstream_providers` row. JSON columns travel as
// strings (the storage layer owns (de)serialization; callers parse with
// their own Json helpers).
struct UpstreamProviderRecord
{
    int32_t id = 0;
    std::string providerKey;
    std::string displayName;
    std::string issuer;
    std::string clientId;
    std::string clientSecret;
    std::string scopes;             // space-separated; default "openid profile email"
    std::string claimsMapping;      // JSON object string; {"sub":..,"email":..,"username":..,"name":..}
    std::string endpointOverrides;  // JSON object string; {"authorize":..,"token":..,"userinfo":..}
    bool enabled = true;
};

class IUpstreamProviderRepository
{
  public:
    virtual ~IUpstreamProviderRepository() = default;

    using ListCallback = std::function<void(std::vector<UpstreamProviderRecord> records)>;
    using FindCallback = std::function<void(std::optional<UpstreamProviderRecord> record)>;
    // ok=false + error carries a stable, loggable error token (e.g.
    // "duplicate_key", "not_found", "db_error"); implementations log details.
    using MutationCallback =
      std::function<void(bool ok, const std::string &error, std::optional<UpstreamProviderRecord> record)>;

    // Admin listing: every row, enabled or not.
    virtual void listAll(ListCallback &&cb) = 0;
    // Login-page listing: enabled rows only.
    virtual void listEnabled(ListCallback &&cb) = 0;
    virtual void findByKey(const std::string &providerKey, FindCallback &&cb) = 0;

    virtual void insertRecord(const UpstreamProviderRecord &record, MutationCallback &&cb) = 0;
    // Full-row update keyed by providerKey (the admin API's resource id).
    virtual void updateRecord(const std::string &providerKey,
                              const UpstreamProviderRecord &record,
                              MutationCallback &&cb) = 0;
    virtual void deleteByKey(const std::string &providerKey, MutationCallback &&cb) = 0;
};

}  // namespace fulla::identity
