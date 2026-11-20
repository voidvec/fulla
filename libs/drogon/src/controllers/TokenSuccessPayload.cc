#include "TokenSuccessPayload.h"

namespace fulla::drogon::controllers::detail
{
namespace
{
Json::Value accessTokenFields(const std::string &accessToken, int64_t expiresIn)
{
    Json::Value json;
    json["access_token"] = accessToken;
    json["token_type"] = "Bearer";
    json["expires_in"] = static_cast<Json::Int64>(expiresIn);
    return json;
}
}  // namespace

Json::Value buildClientCredentialsTokenPayload(
  const std::string &accessToken,
  int64_t expiresIn,
  const std::string &scope
)
{
    auto json = accessTokenFields(accessToken, expiresIn);
    json["scope"] = scope;
    return json;
}

Json::Value buildDeviceTokenPayload(
  const std::string &accessToken,
  int64_t expiresIn,
  const std::string &refreshToken,
  const std::string &scope
)
{
    auto json = accessTokenFields(accessToken, expiresIn);
    json["refresh_token"] = refreshToken;
    if (!scope.empty())
        json["scope"] = scope;
    return json;
}

}  // namespace fulla::drogon::controllers::detail
