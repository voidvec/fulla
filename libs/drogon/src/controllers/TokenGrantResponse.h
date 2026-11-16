#pragma once

#include <drogon/HttpResponse.h>
#include <fulla/common/ports/IMetrics.h>
#include <json/json.h>

#include <memory>

namespace fulla::drogon::controllers::detail
{

enum class TokenGrantResponseKind
{
    AuthorizationCode,
    RefreshToken,
};

// Synchronous HTTP presentation with explicit metrics side effects. The
// controller retains callback ownership and resolves the metrics instance.
::drogon::HttpResponsePtr buildTokenGrantResponse(
  const Json::Value &result,
  TokenGrantResponseKind kind,
  const std::shared_ptr<::fulla::common::ports::IMetrics> &metrics
);

}  // namespace fulla::drogon::controllers::detail
