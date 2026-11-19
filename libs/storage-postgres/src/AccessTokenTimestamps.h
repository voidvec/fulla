#pragma once

#include <fulla/oauth2/model/Dto.h>
#include <fulla/storage/postgres/models/Oauth2AccessTokens.h>

#include <chrono>

namespace fulla::storage::postgres::detail
{

// Private row mapping, not an installed clock or repository interface.
// Zero means unspecified, matching the memory backend's existing defaults.
// Assign both columns even when the resolved value is zero: omitting a column
// would let PostgreSQL round its fractional CURRENT_TIMESTAMP to a future second.
inline void assignAccessTokenTimestamps(
  ::drogon_model::fulla_db::Oauth2AccessTokens &row,
  const ::fulla::oauth2::model::OAuth2AccessToken &token,
  std::chrono::system_clock::time_point capturedNow
)
{
    const auto now =
      std::chrono::duration_cast<std::chrono::seconds>(capturedNow.time_since_epoch()).count();
    row.setIssuedAt(token.issuedAt == 0 ? now : token.issuedAt);
    row.setNotBefore(token.notBefore == 0 ? now : token.notBefore);
}

}  // namespace fulla::storage::postgres::detail
