#pragma once

// Task 17 slice 6 (fulla-sdk-refactor, design.md §6/§8's Data Models
// table): the `Client` aggregate ("clientId/type/secretHash/redirectUris/
// allowedScopes | 单实体"). Wraps the existing OAuth2Client DTO (Task 17
// slice 2, model/Dto.h) with the invariant-checking behavior an aggregate
// is meant to carry (design.md §3.1: "值对象...消除裸 std::string 引发的
// 校验/注入类缺陷"; "聚合边界即事务边界") -- here, mainly scope/redirect-uri
// membership checks that call sites currently duplicate ad hoc (e.g.
// TokenService.cc's exchangeCodeForToken already does a raw
// `client->clientType == ClientType::PUBLIC` check; AuthorizationService,
// once it exists, needs redirect_uri validation and scope-subset
// validation too).
//
// Deliberately a thin wrapper (holds an OAuth2Client by value, does not
// duplicate its fields) rather than a full re-modeling with value objects
// (ClientId/RedirectUri/Scope) -- that is a separate, larger slice per
// Dto.h's own comment. This aggregate's job is to give call sites a
// single place for "is scope X allowed for this client" /
// "is this redirect_uri registered" logic instead of each call site
// re-implementing a linear std::find over allowedScopes/redirectUris.

#include <fulla/oauth2/model/Dto.h>

#include <algorithm>
#include <string>

namespace fulla::oauth2::model
{

/**
 * @brief The `Client` aggregate (design.md's Data Models table): an
 * OAuth2Client DTO plus the membership-check behavior every call site
 * needs (scope allowlist, redirect_uri registry).
 */
class Client
{
  public:
    explicit Client(OAuth2Client dto) : dto_(std::move(dto))
    {
    }

    const std::string &clientId() const noexcept
    {
        return dto_.clientId;
    }

    ClientType clientType() const noexcept
    {
        return dto_.clientType;
    }

    bool isPublic() const noexcept
    {
        return dto_.clientType == ClientType::PUBLIC;
    }

    bool isConfidential() const noexcept
    {
        return dto_.clientType == ClientType::CONFIDENTIAL;
    }

    const std::string &clientSecretHash() const noexcept
    {
        return dto_.clientSecretHash;
    }

    const std::string &salt() const noexcept
    {
        return dto_.salt;
    }

    const std::vector<std::string> &redirectUris() const noexcept
    {
        return dto_.redirectUris;
    }

    const std::vector<std::string> &allowedScopes() const noexcept
    {
        return dto_.allowedScopes;
    }

    // F-017 (RFC 7591 §2): the client's declared token-endpoint auth method
    // (client_secret_basic | client_secret_post | none). Empty/unset
    // preserves the legacy lenient Basic->body fallback.
    const std::string &tokenEndpointAuthMethod() const noexcept
    {
        return dto_.tokenEndpointAuthMethod;
    }

    /// True iff `redirectUri` exactly matches one of this client's
    /// registered redirect URIs (RFC 6749 §3.1.2.3 requires exact match,
    /// not prefix/pattern matching).
    bool isRegisteredRedirectUri(const std::string &redirectUri) const
    {
        return std::find(dto_.redirectUris.begin(), dto_.redirectUris.end(), redirectUri) !=
               dto_.redirectUris.end();
    }

    // P0-7b (#261): dedicated post-logout whitelist (OIDC RP-Initiated
    // Logout §2.3). Empty list = no dedicated registration: end_session
    // falls back to the login redirect_uris (pre-v1.5.0 behavior).
    const std::vector<std::string> &postLogoutRedirectUris() const noexcept
    {
        return dto_.postLogoutRedirectUris;
    }

    bool hasPostLogoutWhitelist() const noexcept
    {
        return !dto_.postLogoutRedirectUris.empty();
    }

    /// True iff `uri` exactly matches a dedicated post-logout registration.
    /// Only meaningful when hasPostLogoutWhitelist() is true.
    bool isRegisteredPostLogoutRedirectUri(const std::string &uri) const
    {
        return std::find(
                 dto_.postLogoutRedirectUris.begin(),
                 dto_.postLogoutRedirectUris.end(),
                 uri
               ) != dto_.postLogoutRedirectUris.end();
    }

    /// True iff `scope` is in this client's allowed-scopes list.
    bool allowsScope(const std::string &scope) const
    {
        return std::find(dto_.allowedScopes.begin(), dto_.allowedScopes.end(), scope) !=
               dto_.allowedScopes.end();
    }

    /// True iff every space-separated scope token in `requestedScope` is
    /// in this client's allowed-scopes list. Empty `requestedScope` is
    /// trivially allowed (no scopes requested).
    bool allowsAllScopes(const std::string &requestedScope) const
    {
        if (requestedScope.empty())
        {
            return true;
        }
        size_t start = 0;
        while (start < requestedScope.size())
        {
            size_t end = requestedScope.find(' ', start);
            if (end == std::string::npos)
            {
                end = requestedScope.size();
            }
            if (end > start && !allowsScope(requestedScope.substr(start, end - start)))
            {
                return false;
            }
            start = end + 1;
        }
        return true;
    }

    /// Access the underlying DTO (e.g. for repository persistence, which
    /// deals in DTOs, not aggregates).
    const OAuth2Client &dto() const noexcept
    {
        return dto_;
    }

  private:
    OAuth2Client dto_;
};

}  // namespace fulla::oauth2::model
