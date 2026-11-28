#include "AuthorizationCodeResponse.h"

#include <drogon/utils/Utilities.h>
#include <fulla/drogon/utils/PortalUrl.h>
#include <json/json.h>

namespace fulla::drogon::controllers::detail
{

::drogon::HttpResponsePtr buildAuthorizationCodeResponse(
  const std::string &redirectUri,
  const std::string &code,
  const std::string &state,
  bool jsonMode
)
{
    // F-020: retain component encoding of code/state and the original URI bytes.
    std::string location =
      redirectUri + ::fulla::drogon::utils::authorizeUrlQuerySeparator(redirectUri) +
      "code=" + ::drogon::utils::urlEncodeComponent(code);
    if (!state.empty())
        location += "&state=" + ::drogon::utils::urlEncodeComponent(state);
    if (jsonMode)
    {
        Json::Value ret;
        ret["code"] = code;
        ret["location"] = location;
        return ::drogon::HttpResponse::newHttpJsonResponse(ret);
    }
    return ::drogon::HttpResponse::newRedirectionResponse(location);
}

}  // namespace fulla::drogon::controllers::detail
