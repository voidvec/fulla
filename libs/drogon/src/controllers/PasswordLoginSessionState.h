#pragma once

#include <cstdint>
#include <string>

namespace drogon
{
class Session;
}

namespace fulla::drogon::controllers::detail
{
// Internal password-login writes only; callers retain authentication policy.
void setPasswordLoginSubject(
  ::drogon::Session &session,
  int32_t internalId,
  const std::string &publicSub
);

void setPasswordLoginClaimsAndRotate(::drogon::Session &session, int64_t authTime);
}  // namespace fulla::drogon::controllers::detail
