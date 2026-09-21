#include <fulla/storage/memory/MemoryTokenRepository.h>
#include <drogon/drogon.h>
#include <chrono>

namespace fulla::storage::memory
{

// Task 27.5: callback aliases now live on the new base interface; bring
// them into scope for the out-of-class method definitions below. The DTO
// aliases are safe at namespace scope HERE (this .cc does not include
// IOAuth2Storage.h, so no oauth2::OAuth2AccessToken clash).
using AccessTokenCallback = ITokenRepositoryBase::AccessTokenCallback;
using RefreshTokenCallback = ITokenRepositoryBase::RefreshTokenCallback;
using VoidCallback = ITokenRepositoryBase::VoidCallback;
using TokenIntrospectionCallback = ITokenRepositoryBase::TokenIntrospectionCallback;
using OAuth2AccessToken = ::fulla::oauth2::model::OAuth2AccessToken;
using OAuth2RefreshToken = ::fulla::oauth2::model::OAuth2RefreshToken;
using TokenIntrospection = ::fulla::oauth2::model::TokenIntrospection;

int64_t MemoryTokenRepository::getCurrentTimestamp() const
{
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

void MemoryTokenRepository::saveAccessToken(const OAuth2AccessToken &token, VoidCallback &&cb)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Ensure P1 fields have default values if not set
    OAuth2AccessToken tokenWithDefaults = token;
    if (tokenWithDefaults.issuedAt == 0)
        tokenWithDefaults.issuedAt = getCurrentTimestamp();
    // F-016: no hardcoded fallback issuer anymore; callers stamp the
    // configured issuer at issuance, and the introspect controller backfills
    // an empty iss.
    if (tokenWithDefaults.notBefore == 0)
        tokenWithDefaults.notBefore = getCurrentTimestamp();

    accessTokens_[tokenWithDefaults.token] = tokenWithDefaults;
    if (cb)
        cb();
}

void MemoryTokenRepository::getAccessToken(const std::string &token, AccessTokenCallback &&cb)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = accessTokens_.find(token);
    if (it != accessTokens_.end())
    {
        if (it->second.expiresAt > getCurrentTimestamp() && !it->second.revoked)
        {
            cb(it->second);
            return;
        }
    }
    cb(std::nullopt);
}

void MemoryTokenRepository::saveRefreshToken(const OAuth2RefreshToken &token, VoidCallback &&cb)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    refreshTokens_[token.token] = token;
    if (cb)
        cb();
}

void MemoryTokenRepository::getRefreshToken(const std::string &token, RefreshTokenCallback &&cb)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = refreshTokens_.find(token);
    if (it != refreshTokens_.end())
    {
        if (it->second.expiresAt > getCurrentTimestamp() && !it->second.revoked)
        {
            cb(it->second);
            return;
        }
    }
    cb(std::nullopt);
}

void MemoryTokenRepository::revokeRefreshToken(const std::string &token, VoidCallback &&cb)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = refreshTokens_.find(token);
    if (it != refreshTokens_.end())
    {
        it->second.revoked = true;
        LOG_DEBUG << "Refresh token revoked: " << token;
    }
    cb();
}

void MemoryTokenRepository::atomicRevokeRefreshToken(
  const std::string &token,
  RefreshTokenCallback &&cb
)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = refreshTokens_.find(token);
    if (it == refreshTokens_.end())
    {
        cb(std::nullopt);
        return;
    }
    if (it->second.revoked)
    {
        // Already revoked - reuse detected!
        cb(std::nullopt);
        return;
    }
    // CAS: mark as revoked and return the data
    it->second.revoked = true;
    cb(it->second);
}

void MemoryTokenRepository::revokeTokenFamily(const std::string &familyId, VoidCallback &&cb)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Revoke all refresh tokens in this family
    for (auto &pair : refreshTokens_)
    {
        if (pair.second.familyId == familyId)
        {
            pair.second.revoked = true;
            // Also revoke associated access token
            auto atIt = accessTokens_.find(pair.second.accessToken);
            if (atIt != accessTokens_.end())
            {
                atIt->second.revoked = true;
            }
        }
    }
    LOG_WARN << "[SECURITY] Token family revoked due to reuse detection: " << familyId;
    cb();
}

// ========== P1: Token Introspection (RFC 7662) ==========

void MemoryTokenRepository::introspectToken(
  const std::string &token,
  TokenIntrospectionCallback &&cb
)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int64_t now = getCurrentTimestamp();

    // 1. Check Access Tokens
    auto it = accessTokens_.find(token);
    if (it != accessTokens_.end())
    {
        const OAuth2AccessToken &accessToken = it->second;

        // Check if token is revoked or expired
        if (accessToken.revoked || accessToken.expiresAt < now)
        {
            TokenIntrospection introspection;
            introspection.active = false;
            cb(introspection);
            return;
        }

        // Token is active, populate introspection data
        TokenIntrospection introspection;
        introspection.active = true;
        introspection.clientId = accessToken.clientId;
        introspection.tokenType = "Bearer";
        introspection.exp = accessToken.expiresAt;
        introspection.iat = accessToken.issuedAt;
        introspection.iss = accessToken.issuer;
        introspection.aud = accessToken.audience;
        introspection.nbf = accessToken.notBefore;
        introspection.sub = accessToken.userId;
        introspection.scope = accessToken.scope;
        // v1.5.0 M1: org binding exposure (design §2.1 item 4).
        introspection.orgId = accessToken.orgId;

        cb(introspection);
        return;
    }

    // 2. Check Refresh Tokens
    auto itRt = refreshTokens_.find(token);
    if (itRt != refreshTokens_.end())
    {
        const OAuth2RefreshToken &refreshToken = itRt->second;

        // Check if token is revoked or expired
        if (refreshToken.revoked || refreshToken.expiresAt < now)
        {
            TokenIntrospection introspection;
            introspection.active = false;
            cb(introspection);
            return;
        }

        // Token is active
        TokenIntrospection introspection;
        introspection.active = true;
        introspection.clientId = refreshToken.clientId;
        introspection.tokenType = "Bearer";  // RFC 7662 says refresh tokens don't have a specific
                                             // type but we use Bearer context
        introspection.exp = refreshToken.expiresAt;
        introspection.sub = refreshToken.userId;
        introspection.scope = refreshToken.scope;
        // v1.5.0 M1: org binding on the refresh branch too.
        introspection.orgId = refreshToken.orgId;
        // Refresh tokens might not have iat/nbf/iss/aud in the current struct, but we return what
        // we have

        cb(introspection);
        return;
    }

    // Token not found
    TokenIntrospection introspection;
    introspection.active = false;
    cb(introspection);
}

void MemoryTokenRepository::incrementIntrospectCount(const std::string &token, VoidCallback &&cb)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = accessTokens_.find(token);
    if (it != accessTokens_.end())
    {
        it->second.introspectCount++;
    }
    if (cb)
        cb();
}

// ========== P1: Token Revocation (RFC 7009) ==========

void MemoryTokenRepository::revokeAccessToken(
  const std::string &token,
  const std::string &revokedBy,
  VoidCallback &&cb
)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // 1. Try to revoke Access Token
    auto it = accessTokens_.find(token);
    if (it != accessTokens_.end())
    {
        it->second.revoked = true;
        it->second.revokedAt = getCurrentTimestamp();
        it->second.revokedBy = revokedBy;
        LOG_INFO << "Access token revoked successfully in memory storage";
    }

    // 2. Try to revoke Refresh Token
    auto itRt = refreshTokens_.find(token);
    if (itRt != refreshTokens_.end())
    {
        itRt->second.revoked = true;
        itRt->second.revokedAt = getCurrentTimestamp();
        itRt->second.revokedBy = revokedBy;
        LOG_INFO << "Refresh token revoked successfully in memory storage";
    }

    // Always return success per RFC 7009 (prevent token probing)
    if (cb)
        cb();
}

// ========== Cleanup ==========

void MemoryTokenRepository::purgeExpired()
{
    // Token-side slice of the original MemoryOAuth2Storage::deleteExpiredData():
    // accessTokens_ / refreshTokens_ sweeps. The auth-code sweep lives in
    // MemoryGrantRepository::purgeExpired() instead (see
    // REPOSITORY_MAPPING.md #32 decision table).
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int64_t now = getCurrentTimestamp();

    for (auto it = accessTokens_.begin(); it != accessTokens_.end();)
    {
        if (it->second.expiresAt < now)
        {
            it = accessTokens_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = refreshTokens_.begin(); it != refreshTokens_.end();)
    {
        if (it->second.expiresAt < now)
        {
            it = refreshTokens_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

}  // namespace fulla::storage::memory
