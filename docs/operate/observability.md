# OAuth2 Observability Design

The system provides Prometheus metrics through the IMetrics adapter and structured, context-aware metric logs for monitoring and troubleshooting.

## 1. Prometheus Metrics

The PromExporter plugin exposes registered collectors at `/metrics`. The IMetrics adapter (`DrogonMetrics`) registers counter and gauge collectors lazily on first use, so they are exported without configuration. It does not convert `[METRIC]` log lines into Prometheus series.

### 1.1 Metric List

The bounded cache counter is the only family with a fixed label whitelist. The four official configurations pre-register it, and it is updated independently of the INFO log level:

| Metric | Type | Labels | Description |
|----------|------|--------------|------|
| `fulla_cache_total` | Counter | `repo`, `outcome` | Cache lookup outcomes; `repo` is `token`, `client`, or `owner_status`; `outcome` is `hit`, `miss`, or `error`. At most nine series; no user, client ID or token labels. |

Custom configurations must add this collector to the existing `drogon::plugin::PromExporter` configuration:

```json
{"path":"/metrics","collectors":[{"name":"fulla_cache_total","help":"Cache lookup outcomes","type":"counter","labels":["repo","outcome"]}]}
```

Preserve any other configured collectors. A missing or incompatible collector leaves authentication operational but does not export this counter. Non-whitelisted label values and invalid increments are rejected without creating series. The Gauge API never registers or mutates `fulla_cache_total`: `setGauge` with that name is a no-op in every collector state (missing, counter, or gauge), so a misconfigured deployment cannot create a gauge series for the reserved family.

Other IMetrics counters and gauges are registered lazily and need no configuration:

| Metric | Type | Labels | Description |
|----------|------|--------------|------|
| `oauth2_requests_total` | Counter | `endpoint`, `status` | Total OAuth2 requests |
| `oauth2_login_failures_total` | Counter | `reason` | Login failures |
| `oauth2_introspect_requests_total` | Counter | `client_id` | Total token introspection requests |
| `oauth2_introspect_errors_total` | Counter | `client_id`, `error` | Introspection errors |
| `oauth2_revocation_requests_total` | Counter | `client_id` | Total token revocation requests |
| `oauth2_revocation_errors_total` | Counter | `client_id`, `error` | Revocation errors |
| `oauth2_active_tokens` | Gauge | — | Current estimate of active (unexpired) tokens |

`oauth2_latency_seconds` (Histogram, labels `operation`, `storage`) remains log-only: drogon's Histogram requires explicit bucket boundaries, and no production call site observes histograms through this port yet.

> All adapter metrics also emit `[METRIC]` structured logs via `LOG_INFO` from `libs/drogon/src/adapters/DrogonMetrics.cc`, independently of the export. The legacy statics in `libs/drogon/src/observability/Metrics.cc` emit the same log format but are not wired to the exporter; their definitions live in `libs/drogon/include/fulla/drogon/observability/Metrics.h`.

### 1.2 Sample Dashboard Panels (Grafana)

Recommended panels (the latency histogram requires a log-to-metric pipeline because histograms stay log-only):

- **QPS & Error Rate**: `rate(oauth2_requests_total[1m])` vs `rate(oauth2_login_failures_total[1m])`
- **P99 Latency**: `histogram_quantile(0.99, rate(oauth2_latency_seconds_bucket[1m]))`
- **Business**: Active tokens trend.

## 2. Structured Logging

The system uses context-aware structured logging that Splunk/ELK can easily collect and analyze.

### 2.1 Audit Logs

Critical security operations (such as token issuance) emit logs tagged with `[AUDIT]`.

**Format**:
`[AUDIT] Action={Action} User={UserId} Client={ClientId} Success={True/False} IP={RemoteAddr}`

**Example**:

```
2026-10-18 10:00:00 INFO [AUDIT] Action=IssueToken User=admin Client=fulla-portal Success=True
2026-10-18 10:05:00 WARN [AUDIT] Action=ExchangeCode User=admin Client=fulla-portal Success=False Reason="Replay Detected"
```

Security-relevant policy denials and social-login issuance use stable action keywords:

- `AUTH_LEGACY_HASH_REJECTED` — login denied because the stored password
  hash is legacy-format and `auth.allow_legacy_hash=false` (#103). WARN
  with the internal user id; migrate the account via password reset or a
  temporary window reopen (docs/operate/configuration-guide.md §10).
- `SOCIAL_LOGIN_TOKEN_ISSUED` — a social login endpoint (GitHub/Google/WeChat)
  issued a first-party token pair (#70). Details carry provider/client/scope/internal id.
- `webauthn_clone_detected` — an assertion presented a non-increasing signCount
  (#142): rejected as a possible cloned authenticator; the credential should be
  de-registered and re-registered.
- `mfa_backup_code_used` — a failed TOTP attempt was rescued by an MFA backup
  code (v1.5.0); the token response carries `backup_codes_remaining`.
- `webauthn_credential_deleted` — a passkey removed via
  `DELETE /api/me/webauthn/credentials/{credentialId}` (v1.5.0; the delete
  predicate binds ownership, so foreign and unknown ids answer the same 404).

Since v1.6.0 the audit trail is also exportable as CSV:
`GET /api/admin/logs/export` (admin role + `audit:read` scope; same filters as
the list endpoint plus a mandatory time window unless `all=true`; cells are
formula-injection hardened; every export is itself audited as `audit_exported`).
Retention and the export row cap are configurable — see
[Configuration Guide §13](configuration-guide.md).

### 2.2 Contextual Logs

Along the request-processing chain, every log line automatically carries a `RequestId` for correlation in distributed tracing.

```cpp
LOG_INFO << "Processing request"; // 输出: [ReqId: abc-123] Processing request
```

## 3. Configuration and Integration

### 3.1 Enabling Metrics

By default the metrics exporter listens on the `/metrics` endpoint (the exporter must be enabled in the Drogon configuration file).

### 3.2 Log Levels

The system uses six log levels throughout, mapping one-to-one to Drogon/Trantor's built-in levels (trace < debug < info < warn < error < fatal):

| Level | Meaning | Typical scenarios |
|------|------|----------|
| `trace` | Finest-grained tracing | Function inputs/outputs, loop iterations, line-by-line execution traces — for deep debugging and pinpointing |
| `debug` | Debug information | Variable values, branch decisions, internal state changes — for troubleshooting during development |
| `info` | Routine information | Service start/stop, key flow milestones, user logins, task completion and other normal business events |
| `warn` | Warnings | Recoverable anomalies, degraded handling, configuration falling back to defaults, resources nearing thresholds — the system still runs normally |
| `error` | Errors | Feature failures, request errors, database connection failures — affect a single operation but the service remains available overall |
| `fatal` | Fatal errors | Severe faults that crash the service or prevent it from running; requires immediate alerting and human intervention |

**Conventions**:

- Production enables `info` and above by default; `trace`/`debug` are enabled dynamically on demand.
- Higher levels log less; `fatal` should be extremely rare.
- `apps/server/config/config.prod.json` defaults to `INFO`, `config.dev.json` defaults to `DEBUG`, and `config.json`/`config.ci.json` default to `DEBUG`.

**Dynamic adjustment**: edit the `app.log.log_level` field in the configuration file; allowed values are `TRACE`/`DEBUG`/`INFO`/`WARN`/`ERROR`/`FATAL` (case-insensitive):

```json
"app": {
    "log": {
        "log_level": "INFO"
    }
}
```

**Code conventions**:

- The domain layer (`libs/common`, `libs/oauth2`, `libs/identity`) **must not** use Drogon's `LOG_*` macros directly; it must go through the `fulla::common::ports::ILogger` port so that unit tests can capture and assert with `FakeLogger`.
- The adapter / infrastructure layer (`libs/drogon`, `libs/storage-*`, `apps/server`) may use `LOG_*` macros directly.
- Six-level mapping: `LogLevel::Trace`→`LOG_TRACE`, `Debug`→`LOG_DEBUG`, `Info`→`LOG_INFO`, `Warn`→`LOG_WARN`, `Error`→`LOG_ERROR`, `Fatal`→`LOG_FATAL`.

> For the test-output minimization strategy (ctest prints only failed cases and a summary by default), see [testing-guide.md](../contribute/testing-guide.md).
