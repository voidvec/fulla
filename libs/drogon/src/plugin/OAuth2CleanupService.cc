#include <fulla/drogon/plugin/OAuth2CleanupService.h>
#include <drogon/drogon.h>
#include <trantor/utils/Date.h>

#include <ctime>
#include <utility>

namespace fulla::drogon
{

OAuth2CleanupService::OAuth2CleanupService(
  std::shared_ptr<::fulla::oauth2::repository::IGrantRepository> grantRepo,
  std::shared_ptr<::fulla::oauth2::repository::ITokenRepository> tokenRepo,
  ::drogon::orm::DbClientPtr auditDbClient,
  bool postgresStorage
)
    : grantRepo_(std::move(grantRepo)),
      tokenRepo_(std::move(tokenRepo)),
      auditDbClient_(std::move(auditDbClient)),
      postgresStorage_(postgresStorage)
{
}

OAuth2CleanupService::~OAuth2CleanupService()
{
    // If shutdown() was called, stop() has already been executed
    // No need to call it again, which avoids accessing the Event loop during
    // teardown
    if (!stopped_ && running_)
    {
        LOG_WARN << "OAuth2CleanupService destroyed without explicit shutdown()";
    }
}

void OAuth2CleanupService::start(double intervalSeconds)
{
    if (running_)
        return;

    // Guard: only start if the event loop is available
    auto loop = ::drogon::app().getLoop();
    if (!loop)
    {
        LOG_WARN << "Event loop not available, OAuth2 Cleanup Service will "
                    "not start";
        return;
    }

    running_ = true;
    interval_ = intervalSeconds;
    LOG_INFO << "Starting OAuth2 Cleanup Service with interval: " << intervalSeconds << "s";

    // Use weak_ptr to prevent SegFaults if Service is destroyed before timer
    // triggers
    std::weak_ptr<OAuth2CleanupService> weakSelf = weak_from_this();

    timerId_ = loop->runEvery(interval_, [weakSelf]() {
        auto self = weakSelf.lock();
        if (self && self->running_)
        {
            self->runCleanup();
        }
    });
}

void OAuth2CleanupService::stop()
{
    // Guard: if already stopped, do nothing (prevents accessing Event loop
    // during teardown)
    if (stopped_)
        return;

    stopped_ = true;

    if (!running_)
        return;

    LOG_INFO << "Stopping OAuth2 Cleanup Service";
    running_ = false;

    // Safely attempt to invalidate timer if loop is still running
    // Use try-catch to handle cases where Event loop is already destroyed
    try
    {
        auto loop = ::drogon::app().getLoop();
        if (loop && timerId_ != 0 && loop->isRunning())
        {
            loop->invalidateTimer(timerId_);
            timerId_ = 0;
        }
    }
    catch (const std::exception &e)
    {
        LOG_WARN << "Exception while invalidating timer: " << e.what();
    }
    catch (...)
    {
        LOG_WARN << "Unknown exception while invalidating timer";
    }
}

void OAuth2CleanupService::runCleanup()
{
    if (!running_ || !grantRepo_ || !tokenRepo_)
        return;

    // Distributed lock: only one instance should run cleanup at a time
    // Try to acquire Redis lock (if Redis is available)
    try
    {
        auto redis = ::drogon::app().getRedisClient("default");
        // SET key value NX EX ttl (acquire lock with TTL)
        int lockTtl = static_cast<int>(interval_ * 0.8);  // 80% of interval
        if (lockTtl < 60)
            lockTtl = 60;

        // Defect 1.10 fix: the Redis callbacks may fire AFTER this service has
        // been destroyed (the command is dispatched on the Redis client's loop
        // and the reply arrives later). Capturing a raw `this` here — while
        // start()'s runEvery timer already uses weak_from_this() — was an
        // implementation inconsistency that could dereference a dangling `this`
        // (use-after-free on this->running_ / this->storage_). Capture a
        // weak_ptr and lock() at the top of each callback, matching start():
        // if the service is gone (or no longer running), skip the cleanup
        // safely. A periodic cleanup is droppable, so weak_ptr (skip-if-gone)
        // is the correct choice rather than shared_ptr (extend-lifetime).
        std::weak_ptr<OAuth2CleanupService> weakSelf = weak_from_this();

        redis->execCommandAsync(
          [weakSelf](const ::drogon::nosql::RedisResult &r) {
              auto self = weakSelf.lock();
              if (!self || !self->running_)
                  return;
              if (r.isNil())
              {
                  // Lock not acquired - another instance is running cleanup
                  LOG_DEBUG << "Cleanup lock not acquired (another instance is running)";
                  return;
              }
              // Lock acquired - proceed with cleanup
              LOG_INFO << "Running periodic data cleanup (lock acquired)...";
              try
              {
                  self->doPurge();
              }
              catch (const std::exception &e)
              {
                  // Recoverable: background task retries on the next tick.
                  LOG_WARN << "Error during OAuth2 cleanup: " << e.what();
              }
          },
          [weakSelf](const std::exception & /*e*/) {
              auto self = weakSelf.lock();
              if (!self || !self->running_)
                  return;
              // Redis not available - run cleanup anyway (single instance mode)
              LOG_INFO << "Running periodic data cleanup (no Redis lock)...";
              try
              {
                  self->doPurge();
              }
              catch (const std::exception &ex)
              {
                  // Recoverable: background task retries on the next tick.
                  LOG_WARN << "Error during OAuth2 cleanup: " << ex.what();
              }
          },
          "SET oauth2:cleanup:lock %s NX EX %d",
          "locked",
          lockTtl
        );
    }
    catch (...)
    {
        // Redis not configured - run cleanup without lock
        LOG_INFO << "Running periodic data cleanup (no Redis)...";
        try
        {
            doPurge();
        }
        catch (const std::exception &e)
        {
            // Recoverable: doPurge() already ran; next tick retries.
            LOG_WARN << "Error during OAuth2 cleanup: " << e.what();
        }
        catch (...)
        {
            // Recoverable: same swallow-and-continue as above.
            LOG_WARN << "Unknown error during OAuth2 cleanup";
        }
    }
}

void OAuth2CleanupService::doPurge()
{
    // Phase 4.2: the old IOAuth2Storage::deleteExpiredData() is now split into
    // per-repository purgeExpired() (design.md §7 / Task 7 A3). Both are
    // synchronous (no callback). Purge grants (auth codes + authorization
    // transactions) then tokens (access + refresh).
    if (grantRepo_)
        grantRepo_->purgeExpired();
    if (tokenRepo_)
        tokenRepo_->purgeExpired();
    // #83: keep the audit_logs partition horizon advancing on the same cycle
    // (inside the distributed lock when one exists -- ensure_audit_partitions
    // is idempotent but not concurrency-safe between instances).
    maintainAuditPartitions();
    // P1-6 (#283): retention drops happen on the same locked cycle. Safe to
    // interleave with partition creation -- retention only ever targets
    // months strictly before the retention window, creation only ever
    // touches current/future months.
    maintainAuditRetention();
}

void OAuth2CleanupService::maintainAuditPartitions(std::function<void()> &&completion)
{
    auto sharedCompletion =
      std::make_shared<std::function<void()>>(std::move(completion));
    // Memory/no-DB deployments have no audit partitioning to maintain, and
    // resolving a DbClient here would be a process-terminating assert.
    if (!postgresStorage_ || !auditDbClient_)
    {
        (*sharedCompletion)();
        return;
    }
    // Raw-SQL exemption ① (db-operations.md): DDL-by-proxy -- the function
    // CREATEs/ATTACHes monthly partitions; no Mapper expression can invoke a
    // SQL function. Fire-and-forget: a maintenance failure logs and never
    // blocks the cleanup cycle.
    auditDbClient_->execSqlAsync(
      "SELECT ensure_audit_partitions() AS created",
      [sharedCompletion](const ::drogon::orm::Result &result) {
          const long created =
            (result.empty() ? 0 : result[0]["created"].as<long>());
          if (created > 0)
              LOG_INFO << "audit_logs partition maintenance created " << created
                       << " new monthly partition(s)";
          (*sharedCompletion)();
      },
      [sharedCompletion](const ::drogon::orm::DrogonDbException &e) {
          LOG_ERROR << "audit_logs partition maintenance failed (next cleanup "
                       "cycle retries): "
                    << e.base().what();
          (*sharedCompletion)();
      }
    );
}

// ---------------------------------------------------------------------------
// P1-6 (#283): audit retention
// ---------------------------------------------------------------------------

namespace
{
// Parse "audit_logs_pYYYY_MM" -> (yyyy, mm). False for anything else
// (DEFAULT, foreign names, malformed).
bool parsePartitionMonth(const std::string &name, int &yyyy, int &mm)
{
    const std::string prefix = "audit_logs_p";
    if (name.size() != prefix.size() + 4 + 1 + 2)
        return false;
    if (name.compare(0, prefix.size(), prefix) != 0)
        return false;
    if (name[prefix.size() + 4] != '_')
        return false;
    for (std::size_t i = prefix.size(); i < name.size(); ++i)
    {
        if (i == prefix.size() + 4)
            continue;  // the '_' separator, checked above
        if (name[i] < '0' || name[i] > '9')
            return false;
    }
    yyyy = std::stoi(name.substr(prefix.size(), 4));
    mm = std::stoi(name.substr(prefix.size() + 5, 2));
    return mm >= 1 && mm <= 12 && yyyy >= 1970;
}

// (yyyy, mm) of `nowSeconds` shifted back by `months` (UTC, month-tuple
// arithmetic only -- no day-level precision needed for a whole-month rule).
std::pair<int, int> cutoffMonth(int64_t nowSeconds, int months)
{
    std::time_t t = static_cast<std::time_t>(nowSeconds);
    std::tm tmUtc{};
#ifdef _WIN32
    gmtime_s(&tmUtc, &t);
#else
    gmtime_r(&t, &tmUtc);
#endif
    int totalMonths = (tmUtc.tm_year + 1900) * 12 + tmUtc.tm_mon - months;
    return {totalMonths / 12, totalMonths % 12};
}
}  // namespace

std::vector<std::string> OAuth2CleanupService::retentionDropCandidates(
  const std::vector<std::string> &partitionNames,
  int retentionMonths,
  int64_t nowSeconds
)
{
    std::vector<std::string> drop;
    if (retentionMonths <= 0)
        return drop;
    const auto cutoff = cutoffMonth(nowSeconds, retentionMonths);
    const int cutoffTuple = cutoff.first * 12 + cutoff.second;
    for (const auto &name : partitionNames)
    {
        int yyyy = 0;
        int mm = 0;
        if (!parsePartitionMonth(name, yyyy, mm))
            continue;  // DEFAULT / foreign / malformed: never touched
        if (yyyy * 12 + (mm - 1) < cutoffTuple)
            drop.push_back(name);
    }
    return drop;
}

void OAuth2CleanupService::maintainAuditRetention()
{
    // Same no-DB guard as maintainAuditPartitions: resolving a DbClient in
    // memory mode is a process-terminating assert.
    if (!postgresStorage_ || !auditDbClient_)
        return;

    // audit.retention_months (custom_config): 0/unset = keep forever.
    int retentionMonths = 0;
    {
        auto config = ::drogon::app().getCustomConfig();
        if (config.isMember("audit") && config["audit"].isMember("retention_months") &&
            config["audit"]["retention_months"].isInt())
            retentionMonths = config["audit"]["retention_months"].asInt();
    }
    if (retentionMonths <= 0)
        return;

    std::weak_ptr<OAuth2CleanupService> weakSelf = weak_from_this();

    // Raw-SQL exemption ① (db-operations.md): DDL-by-proxy — catalog
    // inspection (pg_inherits) has no Mapper expression, and the subsequent
    // DROP TABLE is DDL. Candidate names are re-validated against the
    // strict audit_logs_pYYYY_MM pattern before interpolation, so no
    // identifier injection is possible.
    auditDbClient_->execSqlAsync(
      "SELECT c.relname AS name FROM pg_inherits i "
      "JOIN pg_class c ON c.oid = i.inhrelid "
      "WHERE i.inhparent = 'audit_logs'::regclass",
      [weakSelf, retentionMonths](const ::drogon::orm::Result &result) {
          auto self = weakSelf.lock();
          if (!self)
              return;
          std::vector<std::string> names;
          names.reserve(result.size());
          for (const auto &row : result)
              names.push_back(row["name"].as<std::string>());
          const int64_t now = ::trantor::Date::now().secondsSinceEpoch();
          for (const auto &name : retentionDropCandidates(names, retentionMonths, now))
          {
              LOG_INFO << "audit retention: dropping partition " << name << " (older than "
                       << retentionMonths << " month(s))";
              self->auditDbClient_->execSqlAsync(
                "DROP TABLE IF EXISTS " + name,
                [name](const ::drogon::orm::Result &) {
                    LOG_INFO << "audit retention: dropped " << name;
                },
                [name](const ::drogon::orm::DrogonDbException &e) {
                    LOG_ERROR << "audit retention: failed to drop " << name
                              << " (next cycle retries): " << e.base().what();
                });
          }
      },
      [](const ::drogon::orm::DrogonDbException &e) {
          LOG_ERROR << "audit retention: partition catalog query failed (next cycle retries): "
                    << e.base().what();
      });
}

}  // namespace fulla::drogon
