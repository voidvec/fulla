# Configuration Guide

## 1. Environment Variable Injection

The application supports overriding key configuration items with environment variables. This matters
especially in Docker/Kubernetes environments — sensitive values should not be hardcoded in
`config.json`.

### Supported Environment Variables

| Variable | Description | Config path overridden | Example |
|---|---|---|---|
| `FULLA_DB_HOST` | Database host | `db_clients[0].host` | `postgres` |
| `FULLA_DB_NAME` | Database name | `db_clients[0].dbname` | `fulla_db` |
| `FULLA_DB_PASSWORD` | Database password | `db_clients[0].passwd` | `secret` |
| `FULLA_REDIS_HOST` | Redis host | `redis_clients[0].host` | `redis` |
| `FULLA_REDIS_PASSWORD` | Redis password | `redis_clients[0].passwd` | `secret` |
| `FULLA_PORTAL_CLIENT_SECRET` | Portal client secret (legacy alias: `FULLA_VUE_CLIENT_SECRET`) | `plugins[OAuth2Plugin].config.clients.fulla-portal.secret` | `...` |

> For the full production environment variable list (30+ entries), see the variable table in
> [Production Deployment](deployment.md); this table lists only the six core items of the
> injection mechanism.

### How It Works

1. **Load hook**: at startup, `loadConfiguration()` in `main.cc` first calls `common::config::ConfigManager::load()`, then `ConfigManager::validate()`.
2. **Parse**: the base `config.json` is read into a `Json::Value` object.
3. **Inject**: the environment variables above are checked; when present, the corresponding nodes in the `Json::Value` are updated in place.
4. **Load**: Drogon loads the modified configuration object directly via `drogon::app().loadConfigJson(config)`; no temporary files are written to disk.

### Verification

A dedicated test, `EnvInjectionVerify` (`EnvConfigTest.cc`), guarantees this logic is correct.

## 2. Docker Deployment

The repository ships a `docker-compose.yml` that orchestrates the full stack (see [Docker Deployment](docker-deployment.md) for details).

### Service Stack

- **fulla-frontend**: Vue SPA + Nginx (built from the `frontend-runtime` stage of `deploy/docker/Dockerfile`).
- **fulla-admin**: admin console frontend (built from `frontends/admin/Dockerfile`).
- **fulla-backend**: Drogon backend (built from the `backend-runtime` stage of `deploy/docker/Dockerfile`).
- **fulla-postgres**: PostgreSQL 17 (schema under `apps/server/migrations/` applied at backend startup via `FULLA_AUTO_MIGRATE=true`).
- **fulla-redis**: password-protected Redis 7.
- **fulla-prometheus**: metrics collection.

### Quick Start

```bash
# 构建并启动（在仓库根目录执行）
docker compose -f deploy/docker/docker-compose.yml up -d --build

# 查看日志
docker compose -f deploy/docker/docker-compose.yml logs -f fulla-backend

# 停止
docker compose -f deploy/docker/docker-compose.yml down
```

### Configuration Handling under Docker

`docker-compose.yml` mounts `apps/server/config/config.json` into the container read-only;
the `environment` section injects environment variables (see §1), and at runtime
`ConfigManager::load()` plus environment injection override the file defaults.

## 3. Storage Backend Selection

The OAuth2 plugin's `config.storage_type` determines the persistence backend:

| `storage_type` | Status | Notes |
|---|---|---|
| `postgres` | **Supported (the only production backend)** | Full token persistence, refresh token rotation and reuse detection. |
| `redis` | **Deprecated** | Never persisted refresh tokens historically (`saveRefreshToken`/`getRefreshToken` are no-ops), so rotation and reuse detection silently fail. The mode still starts (for compatibility; it logs an ERROR at startup), but the `refresh_token` grant is rejected with `unsupported_grant_type`. Do not use in new deployments. |
| `memory` | Test only | For unit/integration tests; not for production. |

Target architecture: **Postgres as the storage layer, fronted by an online Redis L2 cache**
(keyspace `fulla:cache:*`, configured via the `cache` block in `config.json` — `enabled` /
`ttl_seconds` / `invalidation_double_delete_delay_ms`; invalidation uses the delayed
double-delete, see `DelayedDoubleDelete`). There is no standalone Redis storage mode.

## 4. Issuer Configuration

`config.metadata.issuer` (custom config) is the single source of truth for the server's issuer
URL. `OAuth2Plugin` reads it once at startup and uses it consistently for:

- the `iss` claim stamped on issued access tokens (authorization_code / refresh_token / client_credentials / device_code grants);
- `iss` in introspection responses (backfilled from the configured value when the stored row carries none);
- the discovery documents (`/.well-known/openid-configuration`, `/.well-known/oauth-authorization-server`).

Constraints:

- Trailing slashes are normalized away automatically; do not rely on them.
- Defaults to `http://localhost:5555` when unset, with a `LOG_WARN`.
- Production deployments **must** configure an `https://` issuer; a plaintext http issuer on a non-loopback host triggers a startup warning.
- Introspection `iss` and the discovery documents' `issuer` are guaranteed byte-for-byte identical (as OIDC Discovery §3 requires).

## 4a. Admin Console Origin & Device Verification URI (#146)

`config.admin_console.url` (custom config) is the ORIGIN (scheme + host [+ port], no
path) of the admin console SPA — `https://admin.example.com` in production,
default `http://localhost:5174` (the dev vite server). It feeds exactly one
consumer today: the RFC 8628 device-authorization response's
`verification_uri`/`verification_uri_complete`, which default to
`{admin_console.url}/admin/devices` — the real device-approval page. Before #146
the default pointed at `/oauth2/device`, a path with no page behind it.

- Explicit override: `config.device_authorization.verification_uri` (full URL)
  wins over the derived default; `verification_uri_complete` is always derived
  (`verification_uri` + `?user_code=<code>`).
- Trailing slashes on `admin_console.url` are normalized away.

## 4b. Forced First-Login Password Change (#145)

Accounts created with `users.must_change_password = true` — the bootstrap admin
(both the random and the `FULLA_BOOTSTRAP_ADMIN_PASSWORD` variants) and users
created/updated via the admin API with `must_change_password: true` — must
change the password before any authorization code is issued:

- `/oauth2/login` answers `200 {"password_change_required": true, ...}` instead
  of issuing a code;
- `/oauth2/authorize` redirects to the frontend login page (which renders the
  change-password form); `prompt=none` answers `error=login_required`;
- `POST /oauth2/consent` answers 403 `AUTH_PASSWORD_CHANGE_REQUIRED`.

The change path is `POST /oauth2/password/change` (session-authenticated,
requires `old_password`, enforces `auth.min_password_length`, revokes all
tokens, clears the flag). `PUT /api/me/password` also clears the flag. An
admin-set flag on an existing account takes effect at that user's next login.

## 5. Client Token-Endpoint Authentication Methods (F-017)

Each client declares, via the `oauth2_clients.token_endpoint_auth_method` column, how it
authenticates at `/oauth2/token`, `/oauth2/introspect`, and `/oauth2/revoke`:

| Value | Semantics |
|---|---|
| `client_secret_basic` | The secret **must** be sent in the `Authorization: Basic` header; a `client_secret` in the body is rejected. |
| `client_secret_post` | The secret **must** be sent in the POST body; the Basic header is rejected. |
| `none` | PUBLIC client; any `client_secret` present is rejected. |
| NULL / empty | Legacy lenient fallback: accepts the Basic header and also a body secret (Basic→body fallback). |

When the field is omitted at creation through the registration/admin endpoints, the following
defaults are stored:

- `PUBLIC` clients → `none` (they have no secret to begin with).
- `CONFIDENTIAL` clients → `client_secret_basic`.

Seed clients declare it explicitly: `fulla-portal` and `fulla-admin-console` → `none`;
`backend-svc` → `client_secret_basic`. Existing clients with NULL values keep their
pre-upgrade behavior; the upgrade does not break existing deployments.

## 6. OIDC prompt / max_age / auth_time (F-022)

The authorization endpoint supports the `prompt` and `max_age` parameters from OIDC Core
§3.1.2.1:

- **`prompt=none`**: no UI of any kind. No session → 302 `error=login_required`;
  consent required → `error=consent_required`. Errors redirect back to the validated
  `redirect_uri` carrying the echoed `state`. Combining `none` with other values (such as
  `none login`) is self-contradictory and returns 400 outright.
- **`prompt=login`**: forces re-authentication even when a session already exists.
- **`prompt=consent`**: forces the consent page even when existing consent already covers the requested scopes.
- **`max_age=<seconds>`**: forces re-authentication if the session's `auth_time` (set at
  login / MFA verification) is older than `max_age`.

`auth_time` and `amr` are persisted with the authorization code and included in the id_token
at redemption: `auth_time` (when greater than 0), `amr` (a JSON array when set), `acr`
(`1` = password only, `2` = MFA). The discovery document advertises
`prompt_values_supported`, `acr_values_supported`, and related claims.

## 7. RP-Initiated Logout (F-027) and Session Invalidation (F-028)

`/oauth2/end_session` (GET + POST) terminates the server-side session. To redirect after
logout, the client must supply a `post_logout_redirect_uri`, and it **must** be one of the
client's registered redirect URIs; the client is identified by the `aud` claim of the
`id_token_hint`. The hint's signature **is** verified (RS256 + kid + iss/exp/sub policy,
issue #78); failed verification is rejected with 400 `AUTH_INVALID_ID_TOKEN_HINT`. Without
a valid hint plus a registered URI, the request is rejected with 400; on success a 302
redirect carries the echoed `state`, and a 200 is returned when no redirect URI is provided.

`POST /oauth2/logout` (the existing API logout) additionally calls `session()->clear()`
(F-028), so the server-side session is terminated together with access-token revocation.

## 8. Authentication Failure Rate Limiting (F-018)

The token / introspect / revoke / device-code polling endpoints share one in-process
sliding-window rate limiter, bucketed by `(client_ip, client_id)`. Once **failed** attempts
within the rolling window (default 60s) reach `max_failures` (default 30), subsequent
requests return **HTTP 429** with a `Retry-After` header and an OAuth2-style
`{error, error_description}` body. Only **failures** count; a single success resets the
counter, so normal load (and integration suites making many consecutive successful requests)
is never rate-limited.

Configured via `custom_config.auth.rate_limit` (all `config*.json` carry the defaults
explicitly):

```json
"custom_config": {
  "auth": {
    "require_pkce_for_public": true,
    "allow_http_redirect_uri": true,
    "rate_limit": {
      "max_failures": 30,
      "window_seconds": 60
    }
  }
}
```

Both keys may be omitted; when the `rate_limit` object is missing, built-in defaults are used
(30 / 60). The limiter is a function-local singleton (`RateLimiter::instance()` from
`libs/common/include/fulla/common/utils/RateLimiter.h`); the four protected endpoints share
one counter table within the same process. This is minimal brute-force / token-probing
protection; multi-instance deployments require shared storage (Redis), which is future work.

## 9. JWKS Key Rotation (#110 — keystore directory)

`plugins.OAuth2Plugin.config.oidc.signing_keystore_dir` points at a **keystore directory**
and is the key-rotation mechanism:

```
<path>/
  2026-09.pem        # one RSA private key per file; FILENAME (minus .pem) = kid
  2026-12.pem
  active_kid         # one-line text file naming the signing kid (e.g. "2026-12")
```

Semantics:

- **Signing** always uses the key named by `active_kid`; the JWT header carries that kid.
- **Verification** routes on the token's header kid across ALL loaded keys, and the JWKS
  endpoint (`/.well-known/jwks.json`) publishes every loaded public key — so outstanding
  tokens keep resolving while their key remains in the directory.
- `GET /api/admin/oidc/keys` reports the live state: every kid, which one is `active`,
  which are merely `published`.
- The keystore takes **precedence** over `FULLA_SIGNING_KEY` / `FULLA_JWT_KEY_PATH` /
  `signing_key_path`, and a configured-but-broken directory is a **hard startup failure**
  (never a silent fallback to a different key source).

**Rotation procedure** (JwkManager is init-once/read-only by design, so each step is a
restart — three restarts per cycle):

1. **Publish**: drop the new `<kid>.pem` into the directory, restart. Both keys are now in
   the JWKS; the OLD key keeps signing. Wait at least the JWKS `Cache-Control` max-age
   (1 h) before step 2 — a strictly-caching RP otherwise cannot resolve the new kid yet.
2. **Switch**: edit `active_kid` to the new kid, restart. New tokens carry the new kid;
   old tokens still verify (old key still published).
3. **Retire**: after the maximum token lifetime has elapsed (access + refresh chain —
   size this from your token TTLs, not the clock of step 2), delete the old `<kid>.pem`
   and restart. Tokens from the old key now fail verification with an unknown-kid error.

Skipping step 3 leaves the old key published forever (harmless, but keeps the compromise
surface open); performing it early invalidates outstanding old-key tokens.

Key generation is standard tooling, e.g.
`openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out 2026-12.pem`.
Protect the directory like any private-key material (file permissions / secret mounts).

## 10. Legacy Password-Hash Migration Window (#103)

fulla verifies passwords exclusively through PBKDF2-SHA256
(`$pbkdf2-sha256$310000$<salt>$<hash>`). The pre-1.0 unsalted-SHA256 format
is **retired and rejected by default** on every path:

- `auth.allow_legacy_hash` is `false` in all shipped configs, and a
  *missing* key also means `false` (assembly-level default). While
  `false`, a login attempt against a legacy-format hash is denied before
  any password verification — with the **correct** password too.
- The denial is a **policy rejection, not a wrong password**: it does not
  advance the account-lockout counter (a username alone must not let an
  attacker lock a legacy user out, and a later window reopen must not be
  blocked by `locked_until`).
- Each denial logs `AUTH_LEGACY_HASH_REJECTED` (WARN) with the internal
  user id — the operator-facing signal for who still needs migration. The
  client always receives the generic `AUTH_INVALID_CREDENTIALS`; nothing
  distinguishes "legacy denied" from "wrong password" from the outside.

**Who is affected?** Only databases seeded before v1.0.1 that still hold
legacy-format rows. Inventory SQL:

```sql
SELECT count(*) FROM users WHERE password_hash NOT LIKE '$pbkdf2-sha256$%';
```

**Migration paths that need no window:**

- **Email password reset** — the reset flow always writes PBKDF2, so a
  legacy user who resets migrates transparently on completion.
- **Administrator-triggered reset** — same effect.

**Change-password does NOT migrate a legacy user**: it verifies the old
password through the same retired branch and therefore fails; use a reset
instead.

**Reopening the window (temporary, login-only):** set
`auth.allow_legacy_hash: true` and restart. Legacy users can then log in
and are transparently rehashed to PBKDF2 on that first successful login
(identity login path only). Because policy rejections never advanced the
lockout counter, no unlock is required before reopening. Close the window
again once the inventory query returns `0`; the window (and the
identity-side rehash code) is scheduled for removal in v1.1.

## 11. WebAuthn / Passkeys (#142)

Passkey registration and authentication perform REAL cryptographic
verification (W3C WebAuthn Level 2 §7.1/§6.1): ES256 only, `fmt="none"`
attestations only, and the assertion signature is checked against the COSE
public key stored at registration time. Configuration:

```json
"webauthn": {
    "rp_id": "localhost",
    "rp_name": "OAuth2 Server",
    "rp_origins": ["http://localhost:5173"]
}
```

- `rp_id` — Relying Party id (the registrable domain the credential is
  scoped to; `localhost` for local development).
- `rp_origins` — STRICT allowlist of accepted `clientDataJSON` origins.
  **Required before the finish endpoints accept anything**: while the list
  is empty (or the key absent), registration/authentication finish fail
  closed. Production must list the exact portal origin(s), e.g.
  `["https://auth.example.com"]`.
- Challenge lifetime is 300 seconds (code-enforced). The REGISTRATION
  challenge is bound to the Bearer subject (the register endpoints sit
  behind the token filter; the SPA sends no session cookie); the
  AUTHENTICATION challenge is bound to the caller's session — the login
  flow requires cookies (`credentials: 'include'`).
- `userVerification` is `required` in both begin responses and enforced
  (`UV=1`) on authentication — authenticators without user-verification
  capability cannot register credentials here (they would never
  authenticate). Sign-count regression is treated as authenticator
  cloning: the assertion is rejected and a `webauthn_clone_detected`
  audit action is recorded.
- **V028 cleared all pre-existing credential rows**: every stored row was
  client-asserted material that never passed attestation verification (and
  authenticateFinish used to accept a bare credential id as proof of
  possession). Users re-register their passkeys after upgrading.
- Single-instance deployments: the subject-bound registration challenge
  store is in-process (same limitation class as the consent_csrf nonce).
  Multi-instance deployments need shared challenge storage (follow-up).
