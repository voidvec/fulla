#pragma once

#include <json/json.h>

#include <cstdint>
#include <string>

namespace fulla::drogon::controllers::detail
{

// CC always includes scope and has no refresh-token or ID-token fields.
Json::Value buildClientCredentialsTokenPayload(
  const std::string &accessToken,
  int64_t expiresIn,
  const std::string &scope
);

// The controller adds any ID token after its existing signing step.
Json::Value buildDeviceTokenPayload(
  const std::string &accessToken,
  int64_t expiresIn,
  const std::string &refreshToken,
  const std::string &scope
);

}  // namespace fulla::drogon::controllers::detail
