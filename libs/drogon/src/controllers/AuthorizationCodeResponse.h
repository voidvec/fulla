#pragma once

#include <drogon/HttpResponse.h>

#include <string>

namespace fulla::drogon::controllers::detail
{

// The controller retains validation, issuance, metrics and callback ownership.
::drogon::HttpResponsePtr buildAuthorizationCodeResponse(
  const std::string &redirectUri,
  const std::string &code,
  const std::string &state,
  bool jsonMode
);

}  // namespace fulla::drogon::controllers::detail
