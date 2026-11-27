#include "TokenGrantResponse.h"

#include <fulla/drogon/error/OAuth2ErrorHandler.h>

namespace fulla::drogon::controllers::detail
{

::drogon::HttpResponsePtr buildTokenGrantResponse(
  const Json::Value &result,
  TokenGrantResponseKind kind,
  const std::shared_ptr<::fulla::common::ports::IMetrics> &metrics
)
{
    const bool hasError = result.isMember("error");
    auto response = ::drogon::HttpResponse::newHttpJsonResponse(result);
    if (hasError)
    {
        const std::string errorCode = result.get("error", "").asString();
        const auto status =
          ::fulla::common::error::OAuth2ErrorHandler::getHttpStatusCode(errorCode);
        response->setStatusCode(status);
        if (metrics)
            metrics->incrementCounter(
              "oauth2_requests_total",
              ::fulla::common::ports::MetricLabels{
                {"endpoint", "token"},
                {"status", std::to_string(static_cast<int>(status))}
              }
            );
        // Direct grant errors preserve the original result and headers. The
        // public error responder also adds defaults and authentication headers.
        return response;
    }

    if (metrics)
    {
        metrics->incrementCounter(
          "oauth2_requests_total",
          ::fulla::common::ports::MetricLabels{{"endpoint", "token"}, {"status", "200"}}
        );
        if (kind == TokenGrantResponseKind::AuthorizationCode)
            metrics->setGauge("oauth2_active_tokens", ::fulla::common::ports::MetricLabels{}, 1.0);
    }
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Pragma", "no-cache");
    return response;
}

}  // namespace fulla::drogon::controllers::detail
