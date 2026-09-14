# OAuth2 Data Persistence

This document describes the OAuth2 plugin's persistence layer design, database schema, Redis key-value structure, and security hardening.

## 1. Design Goals

- **Storage decoupling**: repository interfaces (`IClientRepository`, `IGrantRepository`, `ITokenRepository`, etc. under `libs/oauth2/include/fulla/oauth2/repository/`) abstract over multiple storage backends such as memory, PostgreSQL, and Redis; each backend is assembled as a `*RepositoryBundle` implementation.
- **Data durability**: ensure that critical data — client information, tokens, auth codes — is never lost.
- **Security hardening**: client secrets are never stored in plaintext; salted SHA256 hashing is mandatory.
- **Asynchronous, high performance**: all low-level operations use `execSqlAsync` and `execCommandAsync` on a callback basis, fully exploiting Drogon's non-blocking I/O.

---

## 2. PostgreSQL Storage

Suited to production environments; provides strict, full relational data consistency.

### 2.1 Database Schema

Created by the migration script `apps/server/migrations/V002__oauth2_core.sql` (idempotent, `IF NOT EXISTS`; subsequent migrations add scopes, device codes, lockout, and other columns). The core tables:

#### Client table (`oauth2_clients`)

Stores information about registered client applications.

```sql
CREATE TABLE IF NOT EXISTS oauth2_clients (
    client_id       VARCHAR(50) PRIMARY KEY,
    client_type     VARCHAR(20) NOT NULL DEFAULT 'CONFIDENTIAL',
    client_secret   VARCHAR(100) NOT NULL, -- 存储 SHA256(secret + salt) 的 Hex 字符串
    salt            VARCHAR(50) NOT NULL,  -- 随机盐值
    name            VARCHAR(100),
    redirect_uris   TEXT,                  -- 逗号分隔或 JSON 数组
    allowed_grant_types TEXT               -- 允许的 grant_type 列表
);
```

#### Authorization-code table (`oauth2_codes`)

Short-lived authorization credentials.

```sql
CREATE TABLE IF NOT EXISTS oauth2_codes (
    code            VARCHAR(100) PRIMARY KEY,
    client_id       VARCHAR(50) NOT NULL REFERENCES oauth2_clients(client_id),
    user_id         VARCHAR(50),
    scope           TEXT,
    redirect_uri    TEXT,
    code_challenge  VARCHAR(128),          -- PKCE 支持
    code_challenge_method VARCHAR(10),      -- S256 / plain
    expires_at      BIGINT NOT NULL,       -- Unix Timestamp
    used            BOOLEAN DEFAULT FALSE  -- 防重放攻击
);
```

#### Access-token table (`oauth2_access_tokens`)

```sql
CREATE TABLE IF NOT EXISTS oauth2_access_tokens (
    token           VARCHAR(100) PRIMARY KEY, -- 存 SHA-256(token) 哈希（64 hex），非明文（ADR-0004）
    client_id       VARCHAR(50) NOT NULL REFERENCES oauth2_clients(client_id),
    user_id         VARCHAR(50),
    scope           TEXT,
    expires_at      BIGINT NOT NULL,
    revoked         BOOLEAN DEFAULT FALSE,
    issued_at       BIGINT NOT NULL DEFAULT EXTRACT(EPOCH FROM CURRENT_TIMESTAMP)::BIGINT,
    issuer          VARCHAR(255) NOT NULL DEFAULT '',
    audience        VARCHAR(255),
    not_before      BIGINT DEFAULT EXTRACT(EPOCH FROM CURRENT_TIMESTAMP)::BIGINT,
    introspect_count INTEGER DEFAULT 0,
    revoked_at      BIGINT,
    revoked_by      VARCHAR(50)
);
```

#### Refresh-token table (`oauth2_refresh_tokens`)

```sql
CREATE TABLE IF NOT EXISTS oauth2_refresh_tokens (
    token           VARCHAR(100) PRIMARY KEY,   -- 存 SHA-256(token) 哈希，非明文（ADR-0004）
    access_token    VARCHAR(100) NOT NULL, -- 关联的访问令牌哈希（无外键约束，按值引用）
    client_id       VARCHAR(50) NOT NULL REFERENCES oauth2_clients(client_id),
    user_id         VARCHAR(50),
    scope           TEXT,
    expires_at      BIGINT NOT NULL,
    revoked         BOOLEAN DEFAULT FALSE,
    revoked_at      BIGINT,
    revoked_by      VARCHAR(50)
);
```

---

## 3. Redis Storage (Deprecated)

> **⚠️ The standalone Redis storage mode is deprecated (F-005)**: in this mode the server logs an
> ERROR at startup and rejects the `refresh_token` grant with `unsupported_grant_type`;
> historically, refresh tokens were never persisted in this mode. New deployments must use
> `postgres` plus the optional Redis cache layer (§ Cache Consistency). The key space below is
> kept only as a reference for existing deployments.

### 3.1 Key Pattern Design

All keys are prefixed with `oauth2:` (the cache layer has its own separate `fulla:cache:`
prefix; the transaction-coordination key family `oauth2:transaction:*` is not listed in the
table below).

| Entity | Key format | Type | TTL | Notes |
|------|-------------|------|-----|------|
| **Client** | `oauth2:client:{client_id}` | Hash | none | Fields: `secret` (hash), `salt`, `redirect_uris` (JSON), `allowed_scopes` (JSON) |
| **Auth Code** | `oauth2:code:{code}` | String | 10 minutes | Value: JSON-serialized object |
| **Access Token** | `oauth2:token:{token}` | String | 1 hour | Value: JSON-serialized object |
| **Refresh Token**| `oauth2:refresh:{token}` | String | 30 days | Value: JSON-serialized object |

### 3.2 Sample Data

**Client (Hash structure)**:

```bash
HSET oauth2:client:fulla-portal secret "42a121b66fb9f1d4f73125788f42eb6799110c6aeae5a9a12a2fed5307a0088d" salt "random_salt" redirect_uris "[\"http://localhost:5173/callback\"]"
```

**Auth Code (String value)**:

```json
{
  "client_id": "fulla-portal",
  "user_id": "admin",
  "scope": "openid",
  "redirect_uri": "http://localhost:5173/callback",
  "expires_at": 1735689000,
  "used": false
}
```

---

## 4. Security Hardening

To prevent client-secret exposure in the event of a database breach, the system enforces a strict hashing policy.

### 4.1 Algorithm and Flow

1. **On storage**:
    - Generate a random `salt` (optional, but recommended to reserve in the Postgres schema).
    - Compute `Hash = SHA256(raw_secret + salt)`.
    - Store `Hash` (hex string) and `salt` in the database.

2. **On validation**:
    - The client submits `input_secret`.
    - The system reads `stored_hash` and `salt` from the database.
    - Compute `CheckHash = SHA256(input_secret + salt)`.
    - Compare `CheckHash` with `stored_hash` (case-insensitive).

### 4.2 Code

Implemented in `RedisClientRepository::validateClient` and `PostgresClientRepository::validateClient`.

```cpp
// 核心逻辑示例
std::string input = clientSecret + client->salt;
std::string calculatedHash = drogon::utils::getSha256(input.data(), input.length());
return lower(calculatedHash) == lower(storedHash);
```

---

## 5. Data Lifecycle Management

To keep the database from growing without bound, the system implements an automated expired-data cleanup mechanism.

### 5.1 Policy Overview

| Storage backend | Cleanup strategy | Mechanism | Frequency |
|----------|----------|----------|------|
| **Redis** | **TTL auto-expiry** | Relies on Redis-native `SETEX`/`EXPIRE`; no application-layer involvement. | Real time |
| **PostgreSQL**| **Periodic deletion** | `OAuth2CleanupService` calls the cleanup methods of `IGrantRepository` / `ITokenRepository` to delete expired auth codes and access/refresh tokens. | Default: every 1 hour |
| **Memory** | **Periodic scan** | Same as above; `OAuth2CleanupService` triggers each repository's expiry cleanup. | Default: every 1 hour |

### 5.2 Scheduler Implementation

Cleanup is performed by a dedicated `OAuth2CleanupService` (`libs/drogon/src/plugin/OAuth2CleanupService.cc`), created and started in `OAuth2Plugin::initAndStart`; the interval is controlled by the plugin configuration key `cleanup_interval_seconds` (default `3600`; see `config.json`):

```cpp
cleanupService_ = std::make_shared<OAuth2CleanupService>(grantRepo_, tokenRepo_);
double cleanupInterval = config.get("cleanup_interval_seconds", 3600.0).asDouble();
cleanupService_->start(cleanupInterval);
```

Internally the service uses `drogon::app().getLoop()->runEvery(interval, ...)` for periodic firing, and `weak_from_this()` to guard against callbacks after destruction.

### 5.3 Interface Definition

Cleanup is no longer concentrated in a single `IOAuth2Storage::deleteExpiredData`; it is split per repository — `IGrantRepository` (auth codes) and `ITokenRepository` (access/refresh tokens) each expose their own expired-deletion methods, orchestrated by `OAuth2CleanupService`.

## 6. Storage Backend Selection and the Memory-Backend Warning (F-031)

> **⚠️ The memory storage backend is for testing/development only; it must not be used in production.**

`storage_type="memory"` (see `config.ci.json`) keeps all client / token / code / consent
data in process memory, with **secrets (client_secret) stored in plaintext** (no SHA-256
salted hashing), and:

- All data is lost on process restart (no persistence);
- No multi-user / multi-instance sharing (each process holds an independent copy of the state);
- No transactions, no atomic CAS guarantees (test-stub implementation);
- The memory identity repository always returns `nullopt` from `findByUsername`, so the
  admin login path is unavailable in this mode (`loginAsAdmin()` returns `nullopt`;
  integration tests that depend on it skip cleanly).

**Production deployments must use `storage_type="postgres"`** (Postgres is the only supported
production storage backend; the standalone Redis storage mode is deprecated — see F-005 /
[Configuration Guide §3](../operate/configuration-guide.md)). The only reason the memory
backend exists is to let Windows/macOS CI environments run the DB-independent test cases
(contract tests, pure unit tests, protocol error-envelope tests, etc.) when no Postgres is
available.

## Data Consistency Notes

### Authorization-code single use (anti double-spend)

`consumeAuthCode` guarantees atomicity at the storage layer: PostgreSQL uses `UPDATE ... WHERE consumed = false ... RETURNING`
(a raw-SQL exemption); the Redis backend uses a Lua script; the memory backend uses a mutex. The contract
is covered for all three implementations by `tests/contract/GrantRepositoryContractTest.cc`.

### Cache consistency: delayed double-delete

Write-path invalidation for the Redis L2 cache (key prefix `fulla:cache:`) uses a **delayed
double-delete**: an immediate DEL plus a second, delayed DEL on the event loop (default 200ms,
configurable via `cache.invalidation_double_delete_delay_ms`, clamped to [50,2000]). This covers
the race window where "a reader thread backfills a stale value just before the DEL" (issue #79).
Second-DEL failures are observable via the `fulla_cache_invalidation_failures_total{kind}`
counter (issue #80). The read path is cache-aside: on a miss it falls through to PostgreSQL and
backfills (with TTL as the eventual-consistency backstop).

### Refresh-token families and cascading revocation

Refresh tokens store a family identifier (V008); on detected replay the entire family is
revoked. Revocation can be initiated at three granularities — per token / per client / per
user (admin API and `/oauth2/revoke`).
