#include <fulla/drogon/controllers/HealthController.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>
#include <drogon/drogon.h>
#include <json/json.h>

#include <atomic>

namespace fulla::drogon::controllers
{

OAuth2Plugin *HealthController::resolvePlugin() const
{
    return plugin_ ? plugin_ : ::drogon::app().getPlugin<OAuth2Plugin>();
}

void HealthController::health(
  const ::drogon::HttpRequestPtr & /*req*/,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    // Health check endpoint for monitoring/orchestration systems
    // Returns 200 OK if service is healthy
    Json::Value json;
    json["status"] = "ok";
    json["service"] = "Fulla";
    json["timestamp"] = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                               std::chrono::system_clock::now().time_since_epoch()
    )
                                               .count());
    auto statusCode = ::drogon::k200OK;

    // Check database connectivity (optional - can be expensive)
    try
    {
        auto plugin = resolvePlugin();
        if (plugin)
        {
            json["storage_type"] = plugin->getStorageType();
            json["database"] = "connected";
        }
        else
        {
            json["status"] = "unhealthy";
            json["database"] = "unknown";
            statusCode = ::drogon::k503ServiceUnavailable;
        }
    }
    catch (...)
    {
        json["status"] = "unhealthy";
        json["database"] = "disconnected";
        statusCode = ::drogon::k503ServiceUnavailable;
    }

    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
    resp->setStatusCode(statusCode);
    callback(resp);
}

void HealthController::healthLive(
  const ::drogon::HttpRequestPtr & /*req*/,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    // Liveness: process is running, always 200
    Json::Value json;
    json["status"] = "ok";
    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
    callback(resp);
}

void HealthController::healthReady(
  const ::drogon::HttpRequestPtr & /*req*/,
  std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
)
{
    // Readiness: check DB connectivity
    auto sharedCb =
      std::make_shared<std::function<void(const ::drogon::HttpResponsePtr &)>>(std::move(callback));

    // Memory storage mode: no DbClient is (or should be) configured, and
    // drogon::app().getDbClient() below hits an uncatchable assert()
    // (process-terminating, not a throw -- see IdentityAssembly.cc:65-67 and
    // tests/contract/ContractFixtures.h:77-91 for the documented Drogon trap).
    // Memory storage is a fully supported, intentionally-healthy deployment
    // mode (config.ci.json uses it; plugin->getStorageType()=="memory" is
    // reported as "database":"connected" by the basic /health handler above).
    // So in memory mode the readiness probe reports 200 ready rather than
    // 503 -- returning 503 would make orchestration drain healthy memory
    // pods. Mirrors the storage-type guard convention used by
    // IdentityAssembly::wireIdentityServices() and ContractFixtures.
    auto plugin = resolvePlugin();
    if (plugin && plugin->getStorageType() == "memory")
    {
        Json::Value json;
        json["status"] = "ok";
        json["database"] = "not_configured";
        json["redis"] = "not_configured";
        (*sharedCb)(::drogon::HttpResponse::newHttpJsonResponse(json));
        return;
    }

    try
    {
        auto db = ::drogon::app().getDbClient();
        // Exemption (db-operations.md §3): Connectivity probe, not CRUD.
        // SELECT 1 is the standard DB readiness check pattern.
        db->execSqlAsync(
          "SELECT 1",
          [sharedCb](const ::drogon::orm::Result &) {
              // DB OK - check Redis
              //
              // Drogon's Redis client with no ready connection queues the
              // PING (both callbacks attached) in an internal buffer and,
              // on connect failure, only schedules a reconnect — the
              // queued callbacks are never invoked. Guard readiness with
              // a one-shot timeout so the endpoint always answers; first
              // answer wins (CAS) and cancels the pending timer so a fast
              // answer does not pin the response callback for the full
              // budget (PR #180 review M5). Every branch — including the
              // not-configured catch below — answers through the same
              // guard, so the single-answer invariant is structural.
              auto loop = ::drogon::app().getLoop();
              auto responded = std::make_shared<std::atomic<bool>>(false);
              auto timerId = std::make_shared<::trantor::TimerId>();
              auto answer =
                [sharedCb, responded, loop, timerId](bool redisOk, const char *redisState) {
                    bool expected = false;
                    if (!responded->compare_exchange_strong(expected, true))
                        return;
                    // invalidateTimer must run on the loop that owns the
                    // timer; the redis callbacks may fire on another IO
                    // thread. Cancelling an already-fired timer is a no-op.
                    loop->runInLoop([loop, timerId]() {
                        loop->invalidateTimer(*timerId);
                    });
                    Json::Value json;
                    json["status"] = redisOk ? "ok" : "degraded";
                    json["database"] = "connected";
                    json["redis"] = redisState;
                    auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
                    if (!redisOk)
                        resp->setStatusCode(::drogon::k503ServiceUnavailable);
                    (*sharedCb)(resp);
                };
              try
              {
                  auto redis = ::drogon::app().getRedisClient("default");
                  redis->execCommandAsync(
                    [answer](const ::drogon::nosql::RedisResult &) {
                        answer(true, "connected");
                    },
                    [answer](const std::exception &) {
                        answer(false, "disconnected");
                    },
                    "PING"
                  );
                  *timerId = loop->runAfter(
                    2.0,
                    [answer]() { answer(false, "timeout"); }
                  );
              }
              catch (...)
              {
                  // Redis not configured - that's OK for some deployments
                  answer(true, "not_configured");
              }
          },
          [sharedCb](const ::drogon::orm::DrogonDbException &e) {
              // Readiness probe contract: report DB unavailability as a health
              // status body with HTTP 503 (consumed by orchestration/monitoring
              // systems). This is NOT an Application error response, so it stays
              // a health-status body rather than an Error Envelope, and the HTTP
              // status code is preserved (Requirement 11.4). The raw exception
              // text is an Internal_Detail and is logged server-side only, never
              // surfaced to the client (Requirement 5.3).
              LOG_ERROR << "Readiness probe DB check failed: " << e.base().what();
              Json::Value json;
              json["status"] = "unhealthy";
              json["database"] = "disconnected";
              auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
              resp->setStatusCode(::drogon::k503ServiceUnavailable);
              (*sharedCb)(resp);
          }
        );
    }
    catch (...)
    {
        Json::Value json;
        json["status"] = "unhealthy";
        json["database"] = "unavailable";
        auto resp = ::drogon::HttpResponse::newHttpJsonResponse(json);
        resp->setStatusCode(::drogon::k503ServiceUnavailable);
        (*sharedCb)(resp);
    }
}

}  // namespace fulla::drogon::controllers
