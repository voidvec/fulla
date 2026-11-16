#include <fulla/drogon/controllers/UpstreamProviderAdminController.h>

#ifdef WITH_SOCIAL

#include <fulla/drogon/error/ErrorResponder.h>
#include <fulla/drogon/observability/openapi/OpenApiGenerator.h>
#include <fulla/drogon/services/UpstreamProviderRuntime.h>
#include <fulla/identity/IUpstreamProviderRepository.h>

#include <json/json.h>
#include <trantor/utils/Logger.h>

#include <regex>
#include <sstream>
#include <utility>
#include <vector>

namespace fulla::drogon::controllers
{

namespace
{
namespace openapi = ::fulla::drogon::observability::openapi;
using Record = fulla::identity::UpstreamProviderRecord;
using Callback = std::function<void(const ::drogon::HttpResponsePtr &)>;
using SharedCb = std::shared_ptr<Callback>;

// Same ep factory shape as RoleScopeAdminController (scope-gated admin
// surface; the `admin` super-scope satisfies either via impliedBy).
openapi::EndpointInfo adminEp(
  const char *path,
  const char *method,
  const char *summary,
  const char *description,
  std::vector<std::string> requiredScopes)
{
    openapi::EndpointInfo ep;
    ep.path = path;
    ep.method = method;
    ep.summary = summary;
    ep.description = description;
    ep.tags = {"Admin", "External Auth"};
    ep.requiresAuth = true;
    ep.requiredScopes = std::move(requiredScopes);
    ep.impliedBy = {"admin"};
    return ep;
}

// Public view of a record: client_secret is write-only (bool presence flag).
Json::Value toPublicJson(const Record &record)
{
    Json::Value json;
    json["provider_key"] = record.providerKey;
    json["display_name"] = record.displayName;
    json["issuer"] = record.issuer;
    json["client_id"] = record.clientId;
    json["client_secret_set"] = !record.clientSecret.empty();
    json["scopes"] = record.scopes;
    if (!record.claimsMapping.empty())
    {
        json["claims_mapping"] = record.claimsMapping;
    }
    if (!record.endpointOverrides.empty())
    {
        json["endpoint_overrides"] = record.endpointOverrides;
    }
    json["enabled"] = record.enabled;
    return json;
}

// Mirror of the schema CHECK constraint + the reserved config-static keys
// (a registry row shadowing google/wechat/github would be unreachable: the
// guard resolves config-static providers first).
bool validProviderKey(const std::string &key)
{
    static const std::regex pattern("^[a-z0-9][a-z0-9-]{1,62}[a-z0-9]$");
    if (key == "google" || key == "wechat" || key == "github")
        return false;
    return std::regex_match(key, pattern);
}

// Parses the admin JSON body into a record. Returns "" on success, an
// error code on validation failure. `forUpdate` skips provider_key checks.
std::string parseBody(
  const ::drogon::HttpRequestPtr &req,
  Record &out,
  bool forUpdate,
  std::string &detail)
{
    auto json = req->getJsonObject();
    if (!json || !json->isObject())
    {
        detail = "body must be a JSON object";
        return "VALIDATION_MISSING_REQUIRED_FIELD";
    }
    if (!forUpdate)
    {
        out.providerKey = json->get("provider_key", "").asString();
        if (!validProviderKey(out.providerKey))
        {
            detail = "provider_key must match ^[a-z0-9][a-z0-9-]{1,62}[a-z0-9]$ and not be a "
                     "reserved social key (google/wechat/github)";
            return "VALIDATION_FORMAT_ERROR";
        }
    }
    out.displayName = json->get("display_name", "").asString();
    out.issuer = json->get("issuer", "").asString();
    out.clientId = json->get("client_id", "").asString();
    out.clientSecret = json->get("client_secret", "").asString();
    out.scopes = json->get("scopes", "openid profile email").asString();
    // JSON columns accept either a nested object (canonical for the admin
    // UI) or a pre-encoded string.
    const auto jsonColumn = [&json](const char *field) -> std::string {
        const Json::Value &value = json->get(field, Json::Value());
        if (value.isObject())
        {
            Json::StreamWriterBuilder builder;
            return Json::writeString(builder, value);
        }
        return value.isString() ? value.asString() : std::string();
    };
    out.claimsMapping = jsonColumn("claims_mapping");
    out.endpointOverrides = jsonColumn("endpoint_overrides");
    out.enabled = json->get("enabled", true).asBool();
    if (out.displayName.empty() || out.issuer.empty() || out.clientId.empty())
    {
        detail = "display_name, issuer and client_id are required";
        return "VALIDATION_MISSING_REQUIRED_FIELD";
    }
    if (!forUpdate && out.clientSecret.empty())
    {
        detail = "client_secret is required on create";
        return "VALIDATION_MISSING_REQUIRED_FIELD";
    }
    if (out.issuer.rfind("http://", 0) != 0 && out.issuer.rfind("https://", 0) != 0)
    {
        detail = "issuer must be an http(s) URL";
        return "VALIDATION_FORMAT_ERROR";
    }
    // JSON columns: validate they parse when present.
    Json::CharReaderBuilder builder;
    for (const auto *field : {"claims_mapping", "endpoint_overrides"})
    {
        const std::string value = jsonColumn(field);
        if (value.empty())
            continue;
        Json::Value parsed;
        std::string errs;
        std::istringstream stream(value);
        if (!Json::parseFromStream(builder, stream, &parsed, &errs) || !parsed.isObject())
        {
            detail = std::string(field) + " must be a JSON object";
            return "VALIDATION_FORMAT_ERROR";
        }
    }
    return "";
}

std::string mutationCode(const std::string &token)
{
    if (token == "duplicate_key")
        return "VALIDATION_RESOURCE_CONFLICT";
    if (token == "not_found")
        return "VALIDATION_RESOURCE_NOT_FOUND";
    return "DB_QUERY_ERROR";
}
}  // namespace

void UpstreamProviderAdminController::initApiDocs()
{
    static std::once_flag docsOnce;
    std::call_once(docsOnce, [] { initApiDocsImpl(); });
}

void UpstreamProviderAdminController::initApiDocsImpl()
{
    const char *desc =
      "P2-1 OIDC-in upstream registry. provider_key keys URLs and "
      "oauth2_subject_mappings.provider (immutable after create; empty "
      "client_secret on update keeps the current one; never echoed). "
      "Mutations refresh the login-page provider set immediately.";
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/upstream-providers", "GET", "List Upstream Providers",
              desc, {"roles:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/upstream-providers/{providerKey}", "GET", "Get Upstream Provider",
              desc, {"roles:read"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/upstream-providers", "POST", "Create Upstream Provider",
              desc, {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/upstream-providers/{providerKey}", "PUT", "Update Upstream Provider",
              desc, {"roles:write"}));
    openapi::OpenApiGenerator::addEndpoint(
      adminEp("/api/admin/upstream-providers/{providerKey}", "DELETE", "Delete Upstream Provider",
              desc, {"roles:write"}));
}

namespace
{
SharedCb makeShared(Callback &&cb)
{
    return std::make_shared<Callback>(std::move(cb));
}

void respondError(const ::drogon::HttpRequestPtr &req, const SharedCb &cb, const std::string &code,
                  const std::string &detail)
{
    fulla::common::error::ErrorResponder::respond(
      req, [cb](const ::drogon::HttpResponsePtr &resp) { (*cb)(resp); }, code, detail);
}

void respondJson(const SharedCb &cb, const Json::Value &json, ::drogon::HttpStatusCode status)
{
    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
    resp->setStatusCode(status);
    (*cb)(resp);
}
}  // namespace

void UpstreamProviderAdminController::list(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback)
{
    auto sharedCb = makeShared(std::move(callback));
    if (!runtime_)
    {
        respondError(req, sharedCb, "INTERNAL_ERROR",
                     "upstream registry unavailable (postgres storage required)");
        return;
    }
    runtime_->repository()->listAll([sharedCb](std::vector<Record> records) {
        Json::Value json;
        json["status"] = "success";
        Json::Value arr(Json::arrayValue);
        for (const auto &record : records)
        {
            arr.append(toPublicJson(record));
        }
        json["providers"] = arr;
        json["total"] = static_cast<Json::Int64>(records.size());
        respondJson(sharedCb, json, ::drogon::k200OK);
    });
}

void UpstreamProviderAdminController::getOne(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &providerKey)
{
    auto sharedCb = makeShared(std::move(callback));
    if (!runtime_)
    {
        respondError(req, sharedCb, "INTERNAL_ERROR",
                     "upstream registry unavailable (postgres storage required)");
        return;
    }
    runtime_->repository()->findByKey(
      providerKey, [req, sharedCb](std::optional<Record> record) {
          if (!record)
          {
              respondError(req, sharedCb, "VALIDATION_RESOURCE_NOT_FOUND", "provider not found");
              return;
          }
          respondJson(sharedCb, toPublicJson(*record), ::drogon::k200OK);
      });
}

void UpstreamProviderAdminController::create(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback)
{
    auto sharedCb = makeShared(std::move(callback));
    if (!runtime_)
    {
        respondError(req, sharedCb, "INTERNAL_ERROR",
                     "upstream registry unavailable (postgres storage required)");
        return;
    }
    Record record;
    std::string detail;
    if (const auto code = parseBody(req, record, false, detail); !code.empty())
    {
        respondError(req, sharedCb, code, detail);
        return;
    }
    auto *runtime = runtime_.get();
    runtime->repository()->insertRecord(
      record,
      [req, sharedCb, runtime](bool ok, const std::string &error, std::optional<Record> inserted) {
          if (!ok)
          {
              respondError(req, sharedCb, mutationCode(error), "create upstream provider: " + error);
              return;
          }
          // Refresh the login-page set before answering so a follow-up list
          // or authorize sees the new provider.
          runtime->refresh([sharedCb, inserted = *inserted]() mutable {
              respondJson(sharedCb, toPublicJson(inserted), ::drogon::k201Created);
          });
      });
}

void UpstreamProviderAdminController::update(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &providerKey)
{
    auto sharedCb = makeShared(std::move(callback));
    if (!runtime_)
    {
        respondError(req, sharedCb, "INTERNAL_ERROR",
                     "upstream registry unavailable (postgres storage required)");
        return;
    }
    Record record;
    std::string detail;
    if (const auto code = parseBody(req, record, true, detail); !code.empty())
    {
        respondError(req, sharedCb, code, detail);
        return;
    }
    auto *runtime = runtime_.get();
    runtime->repository()->updateRecord(
      providerKey, record,
      [req, sharedCb, runtime, providerKey](bool ok, const std::string &error, std::optional<Record>) {
          if (!ok)
          {
              respondError(req, sharedCb, mutationCode(error), "update upstream provider: " + error);
              return;
          }
          runtime->refresh([sharedCb, providerKey]() {
              Json::Value json;
              json["status"] = "success";
              json["provider_key"] = providerKey;
              respondJson(sharedCb, json, ::drogon::k200OK);
          });
      });
}

void UpstreamProviderAdminController::remove(
  const ::drogon::HttpRequestPtr &req,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
  const std::string &providerKey)
{
    auto sharedCb = makeShared(std::move(callback));
    if (!runtime_)
    {
        respondError(req, sharedCb, "INTERNAL_ERROR",
                     "upstream registry unavailable (postgres storage required)");
        return;
    }
    auto *runtime = runtime_.get();
    runtime->repository()->deleteByKey(
      providerKey, [req, sharedCb, runtime, providerKey](bool ok, const std::string &error, std::optional<Record>) {
          if (!ok)
          {
              respondError(req, sharedCb, mutationCode(error), "delete upstream provider: " + error);
              return;
          }
          runtime->refresh([sharedCb, providerKey]() {
              Json::Value json;
              json["status"] = "success";
              json["provider_key"] = providerKey;
              respondJson(sharedCb, json, ::drogon::k200OK);
          });
      });
}

}  // namespace fulla::drogon::controllers

#endif  // WITH_SOCIAL
