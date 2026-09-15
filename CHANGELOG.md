# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).
For the versioning policy (when to cut, what to bump, why), see
[Versioning & Release](docs/contribute/versioning-and-release.md).
Changelog entries are written in English (see CONTRIBUTING).

## [1.3.1] - 2026-09-15

### Fixed

- **TOTP entries in authenticator apps showed the internal codename**: adding MFA rendered the entry as `OAuth2Server:<opaque subject>` — the issuer defaulted to the internal codename and the setup endpoint passed the public subject as the account label. The issuer now defaults to `Fulla` (configurable via `custom_config.mfa.totp_issuer` or the `FULLA_MFA_TOTP_ISSUER` env override) and entries are labeled with the username. Existing authenticator entries keep working; re-adding the entry picks up the new name.
- **The bootstrap admin was locked out of the console after the first password change**: the seeded placeholder email (`admin@example.com`) can never be verified and the login flow's email-verified gate sits after the forced password change — the first real login was rejected with `5001 AUTHZ_ACCESS_DENIED: email not verified`. The bootstrap admin now ships `email_verified=true` (the operator's identity is confirmed out-of-band by whoever runs the deploy). Existing v1.3.0 deployments: `UPDATE users SET email_verified=true WHERE username='admin';`
- **A real but unverified mailbox completed the forced password change into a dead end**: the change now delivers the verification email (fire-and-forget) and the response says so, instead of a bare "sign in again". Pair it with the new `FULLA_BOOTSTRAP_ADMIN_EMAIL` variable: when a real admin mailbox and working SMTP delivery are both configured, the bootstrap admin is created unverified and the standard verification flow applies.
- **User-facing names de-branded from the internal codenames**: the `/health` `service` field, the WebAuthn relying-party display name (passkey dialog) and the email sender display name default now read `Fulla`; the GitHub API User-Agent is `fulla-server`.
- **The "Link GitHub Account" button was unreadable in dark theme**: it hardcoded light-theme colors that the dark theme's neutral remap turned into light-on-light; it now uses the themed primary button.

## [1.3.0] - 2026-09-14

### Changed

- **⚠️ Breaking (client identity) — the first-party OAuth2 clients are renamed**: `vue-client` → **`fulla-portal`** (user portal) and `admin-console` → **`fulla-admin-console`** (admin console). The old names described the UI framework instead of the product, and the admin console's client had never been declared in any shipped config. Deployments upgrading from ≤ 1.2.0 must rename the existing rows once so consents and tokens keep pointing at a live client (run before or right after starting the new version — the startup seeder fills in whatever is still missing):
>
> ```sql
> UPDATE oauth2_clients SET client_id = 'fulla-portal' WHERE client_id = 'vue-client';
> UPDATE oauth2_client_scopes SET client_id = 'fulla-portal' WHERE client_id = 'vue-client';
> UPDATE oauth2_clients SET client_id = 'fulla-admin-console' WHERE client_id = 'admin-console';
> UPDATE oauth2_client_scopes SET client_id = 'fulla-admin-console' WHERE client_id = 'admin-console';
> ```
>
> Frontend deployments compiled with `VITE_CLIENT_ID=vue-client` must rebuild with the new id (the Docker images ship `fulla-portal` by default).
- **⚠️ Breaking (environment variables) — `FULLA_VUE_CLIENT_SECRET` / `FULLA_VUE_REDIRECT_URI` renamed to `FULLA_PORTAL_CLIENT_SECRET` / `FULLA_PORTAL_REDIRECT_URI`**, with the old names still honored as deprecated aliases (both map to the same config paths, so an un-migrated `.env` keeps working). New: `FULLA_ADMIN_CONSOLE_REDIRECT_URI` registers the admin console's production redirect URI.

### Added

- **Startup seeding of config-declared OAuth2 clients (#204)**: production had no path from the plugin config's `clients` block to the database (the dev seed SQL is DEV-ONLY), so a freshly deployed server passed credential and email-verified checks at login and then failed code issuance with `3001 VALIDATION_INVALID_INPUT` — the client-existence guard, logging nothing at ERROR level; the admin console's client was missing from every config entirely. A startup seeder now inserts config-declared clients (both first-party clients ship declared in all configs) into `oauth2_clients` / `oauth2_client_scopes` idempotently (`ON CONFLICT DO NOTHING` — config seeds the initial state, admin-API edits stay authoritative across restarts; PUBLIC clients get a random unused secret hash, CONFIDENTIAL plaintext is salted-hashed per the F-002 rule). Production deployments no longer need any manual client SQL.

### Fixed

- **Registration no longer leaves users guessing about email verification**: the success screen used to flash "Account created successfully" for two seconds and auto-redirect to the login form — which rejects unverified accounts — while discarding the backend's "check your email" note. The page now stays put, names the address the verification email was sent to, and links to login explicitly.

## [1.2.0] - 2026-09-14

### Security

This release lands the P0 batch of the 2026-09 line-by-line backend security/correctness audit (46 findings, independently re-verified; the remaining hardening items continue in follow-up PRs).

- **One database migration ships with this release and applies automatically on startup**: V032 adds an `oauth2_codes.nonce` column (idempotent `ADD COLUMN IF NOT EXISTS`) — see the nonce fix below.
- **OIDC nonce was silently dropped (audit P0-1)**: controllers threaded the authorize-request `nonce` into code generation, but no repository wrote it and the column did not exist — `id_token.nonce` was always empty (OIDC Core §3.1.3.7), breaking replay detection for code-flow relying parties. The nonce is now persisted at code issuance and echoed in the exchanged ID token, round-tripped through both the Postgres and the Redis grant repositories.
- **Logout/revocation left refresh tokens alive (audit P0-2)**: the cascade inside `revokeAccessToken` matched the incoming access-token hash against the wrong column (the refresh tokens' own hash), so it never hit — access-token revocation and logout left the paired refresh token usable for up to 30 days. The cascade now joins on `access_token` and additionally kills the whole rotation family, so a rotated refresh token dies together with the (possibly stale) access token presented at logout.
- **Password reset revoked nothing for social/MFA sessions (audit P0-3)**: the reset flow revoked tokens by internal user id while login/MFA/social token rows are keyed by the public subject — the response claimed all sessions were revoked while every public-sub-keyed token stayed valid. Revocation is now dual-keyed (public_sub OR internal id, same shape as account deletion) and outstanding password-reset tokens for the user are consumed.
- **Authorization codes could be minted past the `/oauth2/authorize` boundary (audit P0-4)**: the login/consent/MFA/device paths generated codes without checking that the client exists, the redirect_uri is registered, and the scopes are within the client allowlist — an authenticated open redirect. A shared issuance guard now runs on all four paths; `code_challenge` format is validated per RFC 7636 §4.2; MFA verify maps guard failures to its frozen generic 401 contract (anti-enumeration).
- **Standalone-Redis grant storage dropped PKCE material (audit P0-5)**: the Redis-backed code store lost the code_verifier pair, nonce, and auth context, and skipped redirect_uri matching when the request side was empty; its semantics are now aligned with the Postgres store.
- **Static file serving no longer echoes credentials (audit P1-15)**: `GET /config.json` served the server's own configuration — including DB/Redis credentials in dev and prod layouts. `allow_all` is disabled and `file_types` narrowed to innocuous assets in all shipped configs.
- **Companion hardening (audit P1/P2)**: the exception handler no longer echoes arbitrary CORS origins with credentials (and emits `Vary: Origin`); bearer tokens in URL query parameters are rejected per RFC 9700; the login failure limiter keys on the TCP peer unless `auth.rate_limit_trust_forwarded_for` is explicitly enabled; failed logins burn one PBKDF2 pass to equalize timing across miss/locked/policy-rejection paths; CSPRNG failure no longer mints deterministic tokens; `mfa_token` carries the public subject instead of the internal id; RSA keys under 2048 bits are rejected; userinfo returns 401 instead of a fabricated 200 for an unresolvable subject; cross-client code exchange returns `invalid_grant`; refresh-token introspection no longer fabricates `iat`/`nbf`; an unrecognized `token_endpoint_auth_method` fails closed.

### Added

- **Verification email on self-registration + unauthenticated resend (#198)**: self-registered users previously could not obtain the verification email without first signing in — a deadlock whenever the next step requires a verified address. Registration now sends the email immediately, and a new rate-limited `POST /api/verify-email/resend-by-email` endpoint resends it given only the address; responses are generic to prevent address enumeration (the endpoint is disabled under memory storage).

### Changed

- **Prod config hardening**: `auth.rate_limit_trust_forwarded_for: true` (nginx fronts the prod layout and sets `X-Forwarded-For`); Hodor `sub_limits` now also covers `/oauth2/mfa/verify` and the new resend endpoint.
- **Dev seed scope drift**: the seeded portal client granted scopes via `is_default` (openid + profile) while advertising `openid profile email` — exposed by the new strict scope allowlist. The seed now grants the three advertised scopes explicitly; **existing dev databases need the seed re-applied (or the `email` scope row inserted)**.
- **Deploy templates**: the prod compose `migrate` service now builds the migration image locally instead of pulling a release image (which lagged brand-new migrations), `FULLA_BOOTSTRAP_ADMIN_PASSWORD` passes through to the migrate job, and `AUTO_MIGRATE=false` is the documented default in the env example (the stack runs the dedicated one-shot migrate job instead).

## [1.1.1] - 2026-09-10

### Changed

- **Two database migrations ship with this release and apply automatically on startup**: V030 backfills a generated username for accounts registered with a blank username (all such rows currently store NULL), and V031 lowercases every stored email to the canonical form (registration and login now canonicalize on both code paths — previously a mixed-case registrant could only sign in by typing their email with the exact original casing). **Upgrade note:** V031 aborts (and the server refuses to start) if the database contains two accounts whose emails differ only by case — such rows need manual dedup first; run `SELECT email, count(*) FROM users GROUP BY lower(email) HAVING count(*) > 1;` before upgrading if unsure.

### Fixed

- **Device approval crashed the entire server (browser-e2e audit A-2)**: `POST /oauth2/device/approve` moved its completion callback into both continuations of a single DB lookup; whichever lost the race invoked an empty `std::function` and aborted the event loop — killing the process on every approval miss (DoS-grade: any admin mistyping a device code took the server down). The callback plumbing is now cycle-free, DB faults are distinguished from unknown codes (`DB_CONNECTION_ERROR` vs `VALIDATION_DEVICE_CODE_INVALID`), and the previously untested admin leg gained integration coverage including the full approve→redeem closure.
- **`/health/ready` hung forever with Redis configured but unreachable (A-1)**: the Redis PING was queued with callbacks that never fire on connect failure and the dev config disabled command timeouts. Readiness now always answers — degraded 503 within ~2 s — via an in-controller guard, dev configs enable the native 2 s redis timeout, and the admin dashboard renders its stat cards independently of readiness instead of skeletonizing.
- **Anonymous OAuth authorize flows were dropped after login (U-3)**: an unauthenticated user bounced from `/oauth2/authorize` to the login page landed on the portal dashboard after signing in — the relying party never received its authorization code. The login page now rebuilds the authorize URL (PKCE/state/nonce preserved) for both the password and MFA branches.
- **The SPA callback page burned externally-initiated authorization codes (U-4)**: it redeemed every `?code=` it received even when the flow belonged to another application (always failing PKCE and consuming the one-time code). It now redeems only when the stashed verifier correlates with the callback `state`, and otherwise shows an "authorization complete — return to your application" notice.
- **Blank-username accounts could be deleted with zero confirmation typing (U-2)**: registrations leaving the username blank stored NULL, which the account center read back as an empty string — exactly matching an empty confirm input in the Danger Zone. Registration now generates a `user_<8 hex>` username (with a collision retry; **V030 backfills existing NULL rows**), and the frontend guard rejects an empty confirm input outright.
- **MFA-enabled users were locked out by a loopback redirect_uri mismatch reported as a bad password (U-6)**: the MFA verify leg strictly validates `redirect_uri`, and the seeded client registered only `127.0.0.1` while the portal default sends `localhost` — the rejection surfaced as "incorrect username or password". Both loopback spellings are now registered by the seed and dev configs (production configs unchanged).
- **Self-service password change left a zombie session (U-1)**: the server revoked every token but the SPA kept its optimistic authenticated state, bouncing the user off `/login` into a dashboard of 401s. The session is now dropped locally and the user lands on `/login` with a "sign in again" notice; the guest router guard restores the session before deciding.
- MFA setup renders the backend's `otpauth://` URI as a scannable QR code next to the manual key (U-5); passkey failures are classified instead of masquerading as network errors (M-3); security/login form labels are programmatically associated with their inputs (M-1); `run_server.bat` no longer false-negatives on forward-slash build paths (M-4).
- **⚠️ Breaking (admin API) — `GET /api/admin/oidc/keys` reports the live signing keystore (#110)**

- **⚠️ Breaking (admin API) — `GET /api/admin/oidc/keys` reports the live signing keystore (#110)**: the flat `kid`/`kty`/`alg`/`use`/`key_status` fields are replaced by a `keys[]` array (per-key `kid`/`kty`/`alg`/`use` plus `status`: `active` = signs new tokens, `published` = verification-only during a rotation grace window), `active_kid`, and `key_count`. External admin integrations should read the signing key material from `/.well-known/jwks.json` as before; this endpoint now summarizes rotation state.
- `POST /oauth2/end_session` (like GET) accepts `client_id` as the RP identification when `id_token_hint` is absent (#88): the `post_logout_redirect_uri` must still be registered for that client.
- Error messages now re-translate on locale switch in both frontends (#158): error state stores the normalized error code instead of the resolved string, and banners re-resolve the catalog message at render time through the injected reactive locale getter; text already on screen follows a subsequent language switch (previously it kept the locale it was triggered in).

### Added

- Signing-key rotation keystore (#110): `plugins.OAuth2Plugin.config.oidc.signing_keystore_dir` (`<kid>.pem` files + `active_kid` marker); JWKS publishes every loaded key, verification routes on the JWT `kid`, rotation is a documented three-restart procedure (see `docs/operate/configuration-guide.md` §9).
- Coverage ratchet gate (#105): CI fails when any library's line coverage drops more than 0.5pp below `tools/coverage/ratchet-baseline.json`; re-baselining is a deliberate reviewed PR edit.
- AOT i18n precompilation + bundle-size budget gate (#159): `@intlify/unplugin-vue-i18n` precompiles the UI catalogs and production builds bundle vue-i18n runtime-only (main chunk −118 KB user / −105 KB admin); `scripts/check-frontend-size.mjs` fails when total or entry-chunk JS exceeds the recorded baseline +10% (wired into `_frontend.yml`; the entry-chunk cap doubles as the compiler-back guard).
- Frontend Docker image build smoke (#160): PR CI builds both frontend images (admin image and the `frontend-runtime` stage) with `push: false`, path-filtered to `frontends/**` + `deploy/docker/**`, so image-build regressions fail the PR instead of surfacing at release time.

## [1.1.0] - 2026-09-01

### Security

- **⚠️ Breaking (security hardening) — legacy password hash rejected by default (#103)**: `auth.allow_legacy_hash` now defaults to **false** (a missing config key is also treated as false); unsalted SHA-256 hashes are rejected outright on every path, including old-password verification during a password change. Existing legacy-hash users should migrate via **email/admin-initiated password reset** (a reset writes a PBKDF2 hash, completing the migration) or by temporarily reopening the window (login transparently re-hashes the credential); note that **changing the password alone cannot self-migrate** (the old-password check runs through the retired branch as well). Policy rejections do not count toward account lockout. Dev seeds and all scripts now use PBKDF2 (same passwords); `bench_users.sql` moved to `benchmarks/fulla/seed` (no longer shipped in the production image).
- **⚠️ Breaking (security hardening) — real WebAuthn signature verification (#142)**: registration and authentication now perform genuine W3C WebAuthn Level 2 verification (ES256 only, fmt="none"; challenge binding with 300 s TTL and unconditional consumption; UV required; signCount clone detection). **V028 purges the existing credential table** (all pre-existing rows are unverified client-asserted material — users must re-register their passkeys); **`webauthn.rp_origins` is now a required config key** (the finish endpoints fail closed when it is absent); the old request-body contracts (`{credential_id, public_key}` registration / bare credential_id login) are rejected, closing the hole that allowed impersonation with nothing but a credential_id. New error codes `WEBAUTHN_INVALID_ATTESTATION` (3015) and `WEBAUTHN_CHALLENGE_MISMATCH` (3016).
- **⚠️ Breaking (security hardening) — social login response shape change (#70)**: `/api/{google,wechat}/login` now issues a first-party token pair instead of returning the provider profile (token rows store **public_sub** — this also fixes existing GitHub tokens 404ing across the /api/me family); the GitHub endpoint spec was corrected accordingly. All three providers now auto-create an account on first login (global switch `external_auth.auto_create_on_first_login`, default true; when disabled, an unlinked login returns 403 `AUTH_SOCIAL_ACCOUNT_NOT_LINKED` (5003)); issuance is recorded via the `SOCIAL_LOGIN_TOKEN_ISSUED` audit event (not a consent row).

### Fixed

- **Self-registration / admin-created user authorize→consent 500 (#143)**: both creation paths now write the (local, id) subject mapping (consent's getInternalUserId sole resolution path); a one-shot V027 backfill plus startup self-heal turns "every user has a local mapping" into a startup invariant.
- **WebAuthn hardening (#142 review round)**: crossOrigin=true assertions rejected (this RP does not support cross-origin ceremonies); the registration challenge store sweeps lazily (entries abandoned after begin no longer linger); signCount clone detection switched to an atomic compare-and-set (concurrent assertions with the same credential no longer both pass); `social_token_client_id` is checked for registration before issuance.
- **AdminBootstrapper first-boot admin creation now sets `salt`** (users.salt NOT NULL — the true-fresh-DB first-boot path previously always failed; latent defect exposed by a new test).
- **`setup-database` root-cause classification and self-heal**: precise fix commands per cause — unreachable / authentication failure (wrong password or missing role; anti-enumeration makes them indistinguishable) / database cannot be dropped (in use or not owner) / missing CREATEDB; on Linux/WSL CREATEDB is auto-granted via `sudo -u postgres psql` and retried; test scripts give explicit skip guidance when the staged config is missing.

### Added

- **Google/WeChat login loop closure (#70)**: service-layer four-state account creation/binding, `SocialCallbackPage` (/callback/google, /callback/wechat), Google button on the login page (gated by `VITE_GOOGLE_CLIENT_ID`).
- **WebAuthn support surface and ops docs (#142)**: rp_id/rp_origins configuration, the single-instance challenge-store limitation, the cookie contract (the authentication flow requires credentials:'include').
- Helm `secrets.bootstrapAdminPassword` pass-through (#103).

## [1.0.1] - 2026-08-30

### Security

- **License change: MIT -> AGPL-3.0**. Starting with this version the project
  is licensed under the GNU Affero General Public License v3.0; the v1.0.0
  release remains MIT. The core stays fully open source — individuals,
  study, self-hosting and internal use are unaffected. Vendors offering the
  software (modified or not) as a hosted service must release their
  modifications per AGPL-3.0 §13, or obtain a commercial license
  (open-core: enterprise features are distributed separately). A CLA is
  required for contributions so the project can keep dual-licensing.
- **consent endpoint authentication closure (F1)**: `POST /oauth2/consent` was previously completely unauthenticated (knowing a user_id was enough to approve or deny on the user's behalf). It now enforces four gates: an authenticated session, user_id bound to the session identity, a server-generated random one-time CSRF nonce (10-minute TTL), and redirect_uri registration validation on the deny branch (fixing an open redirect). The SPA ConsentPage sends back `consent_csrf`. New error codes `AUTH_SESSION_REQUIRED` (4007) and `VALIDATION_PASSWORD_TOO_SHORT` (3014).
- **bootstrap admin (#103)**: on first start, when no admin-role user exists, the server auto-creates `admin` (PBKDF2-SHA256; password from `FULLA_BOOTSTRAP_ADMIN_PASSWORD` or randomly generated and printed to the log once). The dev seed `admin/admin` remains dev-only.
- **password policy (#103)**: registration gains server-side minimum-length validation (`auth.min_password_length`, default 8; the previously hardcoded 8 on change/reset now flows through the same config); new error code `VALIDATION_PASSWORD_TOO_SHORT`.
- **legacy hash migration gate (#103)**: `auth.allow_legacy_hash` (default true during the migration window) controls the unsalted SHA-256 verification path; close it once migration is confirmed complete.

### Changed

- `POST /oauth2/consent` responses add 400/401/403 (openapi synced); deny returns 400 instead of 302 for an unregistered redirect_uri.
- `/api/register` password parameter adds minLength 8; the too-short error on change/reset switches from `VALIDATION_FORMAT_ERROR` to `VALIDATION_PASSWORD_TOO_SHORT`.
- Docs: README/deployment guide drop public credential examples; admin creation switches to the bootstrap flow.

### Documentation

- **Docs governance wrap-up (PR #114/#116, governance doc v4, [docs/documentation-governance.md](docs/documentation-governance.md))**: the user-facing docs tree restructured into seven audience zones (evaluate / integrate / deep dives / operate / contribute / benchmark / adr); 12 ADRs + three trust archives (compliance due diligence, rename impact, repository audit) checked in; fulla.dev launched as a Docusaurus bilingual site — **English primary** (`docs/` is the English canonical) + `/zh-CN` Chinese site (translation tree `website/i18n/zh-CN/`, same structure and URLs, navbar switch); three new deep dives (token lifecycle / session management / multi-tenancy); the site ships offline Chinese search and a dead-link gate. Docs change obligation: **dual-write in the same PR** (en/zh in sync, see CONTRIBUTING).

## [1.0.0] - 2026-08-26

> **Series reset note:** the project was **renamed from authforge to fulla**
> (Fulla is the keeper of Frigg's secret coffer in Norse mythology), and the
> version series **resets to 1.0.0** with this release — the rename is
> treated as a new product identity; SemVer constrains package identities,
> not the repository (the PyPI package `fulla-oauth2`, the
> `ghcr.io/voidvec/fulla-*` images, and the CMake package `fulla` are all
> new identities). Prior history is preserved in the authforge-era
> [1.0.0]–[1.4.1] entries below (git commit history fully retained).

### Rename & Branding

- **Repository-wide rename authforge → fulla**: C++ namespaces and public
  header paths (`#include <fulla/...>`), CMake packages and targets
  (`fulla::*`), binaries (`fulla-server` / `fulla-tests`), the Go module
  path, the Helm chart, and the benchmark facility; the api-diff baseline
  was regenerated for the new symbol surface (179 headers, ratified with
  `--force`, PR #94).
- **Environment variable prefix unified to `FULLA_*`** (previously
  `OAUTH2_*`, 903 occurrences; protocol class names such as `OAuth2Plugin`
  are unchanged).
- **Infrastructure naming normalized**: database/role `fulla_db` /
  `fulla_user`, container names `fulla-*`, Redis key prefix `fulla:cache:`,
  authforge-prefixed Prometheus metrics renamed to `fulla_*` (`oauth2_*`
  functional metric names unchanged) (upgrading invalidates the whole cache
  at once — expected one-time behavior).
- **Frontend branding**: Fulla Admin / Fulla, package names `fulla-admin` /
  `fulla-user`.
- **Repository governance professionalized (PR #93)**: AI-tool workspaces
  untracked, kiro design docs moved to `docs/history/design/kiro-specs/`,
  `.claude/` is the single authoritative rules source.

### Compatibility

- **C++ SDK consumers**: breaking (include paths, namespaces, and the CMake
  package name all changed); migration is essentially one sed pass (mapping
  table in `docs/adr/rename-impact-fulla.md` §3).
- **Existing JWTs / sessions / database schema**: fully compatible (the
  protocol surface carries no project name; table names and migrations are
  unchanged).
- The Python SDK package is renamed to `fulla-oauth2`; the old
  `authforge-oauth2` is discontinued.

## [1.4.1] - 2026-08-24

Security and bug-fix release (no new API surface): a batch of High-severity
security fixes + social-session fixes + cache-consistency hardening. All
changes are defensive tightening or fixes of existing behavior; no breaking
changes.

### Security Fixes

- **#78 [High] `/oauth2/end_session` no longer trusts an unsigned id_token_hint**: the hint must now pass `JwkManager::verifyJwt` (newly exported symmetric verification entry: strict RS256, kid matching, EVP_DigestVerify, iss/exp/sub policy) before it can influence the logout decision; verification failure / expiry / issuer mismatch / mismatch with the browser session subject → 400 `AUTH_INVALID_ID_TOKEN_HINT` (new ErrorCatalog 4006, Error Envelope), returned before any fan-out. Previously, knowing any user's public sub was enough to force-logout that user across all RPs. Note: under strict semantics, replaying an old id_token_hint after a restart (ephemeral keys) or a key rotation returns 400 — a legitimate RP should hold an unexpired token before logout (window bounded by the access TTL).
- **#79 [High] Redis cache-aside refill race**: write-path invalidation switched to a shared **delayed double delete** (immediate DEL + a second DEL 200 ms later on the event loop; `cache.invalidation_double_delete_delay_ms` configurable, clamped to [50, 2000]), closing the window where a stale row read on miss is refilled after the immediate DEL and pins a secret/role for a full TTL; the refill side keeps plain SET (NX adds nothing — rationale in design doc §5.4 addendum).
- **#54 [High] social login bypass for soft-deleted users**: the main fix landed in 89c96341 (four-state SocialLinkStatus, deleted_at filtering, locked rejection, full coverage of the MFA/self-service/consent chains); closed here together with test regressions.
- **#80 [Medium] failed invalidation DELs are observable**: every failed DEL attempt is counted into `authforge_cache_invalidation_failures_total{kind}` (IMetrics port) with WARN→retry→ERROR; soft-fail semantics unchanged.

### Bug Fixes

- **#69 GitHub social token hashing unified**: social-issued access/refresh tokens are now stored hashed (same treatment as token-endpoint tokens); social sessions recover from "401 on every request" to working (userinfo / introspect / refresh / revokeTokenFamily all pass). Existing plaintext tokens were already unusable and are simply invalidated (no migration).
- **#75 `/api/me/social` numeric-dispatch branch e2e coverage**: unblocked by #69; new social-token-driven link → list → unlink full-chain HTTP tests.
- **memory-storage `client_type` config key**: `MemoryClientRepository` read only `"type"` while all five configs wrote `"client_type"`, so in memory mode the vue-client configured as PUBLIC silently became CONFIDENTIAL and rejected token requests with an empty secret. The loader now prefers the canonical key (`"type"` kept as fallback).

### SDK / API Surface

- `JwkManager::verifyJwt` (newly exported method) and
  `authforge/storage/redis/DelayedDoubleDelete.h` (newly exported header)
  are both additive; the api-baseline was ratified. No breaking changes.
- `openapi.yaml`: `/oauth2/end_session` description and 400 response updated
  to the verification semantics (ErrorEnvelope schema reference); version
  synced to 1.4.1.

## [1.4.0] - 2026-08-24

Fourth formal release since v1.3.0; the main line is **performance
engineering** (PR #64 merge chain): non-code performance measures (bench-tier
PG instance tuning / pool 64 / cache-on / session TTL=30 bounded retention)
+ wave-1/2 read-cache code optimizations (validateClient cache-line
validation, user profile/roles read caching, token revocation+read merged
into a single EVAL round trip, userinfo reads piggybacking on MGET) +
V025/V026 schema optimizations + deployment default PG 15→17 + competitor
benchmark facility M0–M3 (Keycloak 26.7.1 / Ory Hydra v26.2.0 / Zitadel
v4.17.1 same-environment suites and four-product comparison). **2026-08-23
TTL=30 re-run across the four products: leading in all five comparison
scenarios** (S1 2.1x / S2 2.6x / S3 2.0x / S5 1.9x / S6 1.5x, see
`benchmarks/competitors/results/COMPARISON.md`).

### ⚠️ Breaking / Required reading before upgrading

- **Deployment default PostgreSQL 15 → 17** (`deploy/docker/docker-compose{,.prod,.debug}.yml`, Helm `values.yaml`): motivated by aligning with the benchmark environment (fairness D1: four products on the same PG version) and by client-side libpq 17.x alignment. **An existing 15 data volume cannot start on 17 directly** (a PG major release refuses to mount an old data directory; the database container would restart-loop). Before upgrading you must follow [docs/operate/postgresql-major-upgrade.md](docs/operate/postgresql-major-upgrade.md) for a dump/restore (or pg_upgrade). AuthForge's own benchmark difference between 15 and 17 is within the noise band — this upgrade is not about our own throughput; deployments in no hurry can pin the image tag back to `postgres:15-alpine` (the application is fully compatible with 15).
- **`AuditLogs` ORM model signature change** (V025 `audit_logs` partitioning): the primary key changed from the single `id` column to a composite `(id, timestamp)` key; the drogon-generated model's `getPrimaryKey()` returns `std::tuple<int64_t, int64_t>` (was `int64_t`), `primaryKeyName` becomes `std::vector<std::string>` (was `std::string`), and `setTimestampToNull()` is removed (`timestamp` is now NOT NULL). Zero in-repo callers are affected (api-diff ratified via `--force` and archived); this is only breaking for external code that compiles the `storage-postgres` model headers directly.

> **Version-number decision (explicitly recorded):** the "dependency major
> bump → MAJOR" row of versioning policy §2 targets compile/link-surface
> breaks; the PG17 change is a **deployment-default alignment** (the
> application is compatible with both 15 and 17, and the runbook provides a
> pin-back path), and the AuditLogs signature change has zero in-repo
> callers — neither constitutes an SDK source-level API break (api-diff
> v1.3.0 baseline: zero drift). Decided per the explicit trade-off spirit of
> policy §3 to proceed within a **MINOR** with prominent labeling
> (2026-08-24).

### Added

- **Competitor same-environment benchmark facility (M0–M3)**: parameterized shared facility + officially-recommended config suites for Keycloak / Ory Hydra / Zitadel (S1 discovery / S2 client_credentials / S3 introspect / S5 refresh / S6 userinfo ladder + GC-jitter long runs + cold start + RSS sampling), `run-comparison.sh` one-shot serial run of all four products + `gen-comparison.py` aggregation with no hand-filled numbers. Honest verdicts shipped with the data (S5 methodology artifact fixed, GC-jitter claim closed — same host noise for all four products, full-stack RSS measurement caveats).
- **wave-2 read-cache code optimizations**: `validateClient` validates the cache line directly (P0); user profile/roles Redis read caching (P1); token revocation flag + read value merged into a single EVAL round trip (P2); userinfo read piggyback memo on MGET (P4). A/B measured significant S3/S6 improvements (S6 comparison-table methodology: 18.1k→49.3k QPS).
- **Write-path cache invalidation hooks**: admin user/client write paths and the GitHub binding path invalidate the corresponding cache keys after DB commit (dual subject forms, commit-time alignment, `resetClientSecret` covered).
- **V025 `audit_logs` monthly RANGE partitioning + BRIN**: with a DEFAULT partition fallback and an `ensure_audit_partitions()` rolling maintenance function (idempotent, safe to replay across the whole db-reset chain).
- **V026 token-table redundant index cleanup**: DROP the three single-column token indexes duplicating the PK (INSERT index maintenance 8→6; review confirmed no query pattern depends on them).
- **opt-in LTO build presets**: `linux-release-lto` and friends + bench image preset pass-through (`AUTHFORGE_CMAKE_PRESET`); default presets unaffected.
- **Non-code performance measures landed (bench tier)**: PG instance tuning (-c flag overrides), PG/Redis pools 25→64 (pool-scan verdict), cache-on, `reuse_port=true`, session TTL 120→30 bounded-retention profile (session retention tax -54%, RSS halved), `auto_batch=true` same-day A/B verdict.
- **Four-product comparison re-run 2026-08-23 TTL=30**: the public tables migrated from stale session-methodology numbers to TTL=30 + production-image LTO methodology; Keycloak S6 pool-expiry structural defect fixed (`run-all.sh` recasts the user pool before S6).
- **README performance badge + "how to reproduce" section (P1 outreach item)**: bilingual badge links to COMPARISON.md as the single source of truth, five-scenario table + honest caveats (WSL2 lower bound, SDK/container methodology distinction, no GC-jitter claim), reproduction commands and methodology doc entry points.

### Changed

- **CI Linux matrix leg service containers PostgreSQL 15 → 17** (`_build-test.yml`): CI now covers the deployment-target major version (including V025/V026 DDL behavior on 17); `docs/contribute/ci-cd-guide.md` service table synced.

### Fixed

- Docs drift cleanup (issue #84): PG15 leftovers in the tech-stack table and deployment verification checklist, stale "install from source before first release" notes in the two SDK READMEs (PyPI/nested tags live since v1.3.0), and the wave-2 plan risk-table's "EVAL single key, no cross-slot" misnote (the implementation uses two keys — CROSSSLOT under Redis Cluster; hash-tag to the same slot or split before clustering).
- V025 partition boundaries anchored to UTC — creation/rolling functions no longer depend on the session time zone (047c4d8f).
- Pool-scan script restores swapped-out configs; overlay raises `max_connections` (23ccdc44).
- docker-stats collection bash 5.2 glob regression; same-session RSS filter methodology fix (6951be0d, 3ca37211).
- CI naming validator requires the `[Priority]` segment in test names (68b929fb).

## [1.3.0] - 2026-08-22

Third formal release since v1.2.0, 21 commits. Two main lines: **C1 client
SDKs** (PR #65: Python `authforge-oauth2` + Go — openapi-generated surface +
hand-written auth layers [client_credentials auto-refresh /
authorization_code+PKCE], `regen_clients.py` drift gate + `clients-sdk.yml`
CI + release wiring) and **B2 social account link/unlink** (PR #68:
self-service portal `GET/POST/DELETE /api/me/social/links[/{provider}]`
three endpoints + identity-layer `SocialLinkService` orchestration +
last-credential guard + user-portal Connected Accounts card; including two
review-fix rounds). Purely additive minor, no breaking changes.

### Added

- **clients**: `regen_clients.py` — SDK regeneration + drift-gate tool (pins generator versions, `--check`, pyproject↔cmake version-linkage validation)
- **clients**: Python client SDK (M1) — openapi-python-client generated surface + hand-written auth layer (client_credentials auto-refresh/401 retry, authorization_code+PKCE, introspect Basic)
- **clients**: Go client SDK (M2) — oapi-codegen generated surface + x/oauth2 clientcredentials (AuthStyleInHeader) + authcode/PKCE
- **identity**: `SocialLinkService` + `ISocialAccountRepository` link/unlink repository methods (listForUser excludes `provider='local'` seed rows; 27 unit tests)
- **self-service**: `/api/me/social/links` three endpoints (profile scope + `WITH_SOCIAL` conditional registration; numeric-dispatch user resolution makes GitHub social sessions work; 11 HTTP integration tests + 4 audit events)
- **openapi**: social link endpoints into the spec + SDK regeneration (SocialLinkEntry/List/Result schemas; oasdiff purely additive; v1.3.0 five-point version-source sync)
- **user-portal**: Connected Accounts card + GitHub link callback branch (`state=link` short-circuits before login + session recovery after the full-page round trip; 6 e2e)

### Fixed

- **clients**: Go I3 handles the pointer-typed Issuer in the generated discovery model
- **clients**: the closing path for m2m clients (PR review)
- **self-service**: link accepts only the JSON body's `code` (kept out of access logs); unlink response adds `subject`
- **identity**: `SocialLinkService` separates states for a missing repo dependency vs an illegal provider (assembly defect → 500-class, #74)
- **social-links**: independent review round W1–W4 + S1–S4 — link branch no longer falls through to login (no automatic account creation), Google/WeChat empty-subject guards, openapi disclosure that unlink does not revoke existing sessions, `linked_at` ISO-8601 (Safari-parseable), UNIQUE conflicts classified by constraint name (locale-independent)

### CI/Build

- `clients-sdk.yml` (new): PR path-filtered SDK drift gate + dual-language unit tests; `release.yml` adds the sdk-python job (PyPI publish double-gated on tag+secret — the first release needs a manually registered project and `PYPI_API_TOKEN` configured), and the github-release job pushes the Go nested tag `clients/go/v<version>` (subdirectory modules are invisible to the root tag)

### Test-infra

- `drogon_macro_bool_check.py`: static check for bare `||`/`&&` inside drogon `CHECK`/`REQUIRE` macros added to CI static-checks (#76, PR #77); testing guide gains the [MUST] assertion-style rules

## [1.2.0] - 2026-08-17

Second formal release since v1.1.0, 61 commits. Main lines: #43
resource-scope authorization model, user-management CRUD completion (A2,
PR #52), OIDC back-channel logout backend (B1, PR #50), OpenAPI spec
governance and breaking-change gate (A1/M0, PR #63), benchmark M2–M4
load-bearing validation (PR #48), and the #53–#60 user-management security
hardening batch (PR #62).

### ⚠️ Breaking (security hardening)

> These changes tighten previously lax (or wrong) behavior. Per the
> versioning policy ([§3 gray-zone trade-offs]
> (docs/contribute/versioning-and-release.md#3-the-security-hardening-gray-zone--an-explicit-trade-off-statement))
> they proceed within a MINOR, not a MAJOR. Downstreams relying on the old
> behavior must migrate accordingly.

- **`/api/admin/*` now requires the `admin` scope** (#43, F-010): admin-surface routes are gated by the declarative `(path,method)→scope` registry. Calls holding only the RBAC admin role but whose access token lacks the `admin` scope return 403 (RFC 6750 `insufficient_scope`). Migration: admin-surface clients request the `admin` scope in the token request.
- **Soft-delete contract enforced across the whole chain** (#54): soft-deleted users can no longer obtain new tokens or sessions via social login, MFA login completion, self-service, or any other path.
- **`backchannel_logout_uri` validation tightened** (#57): https enforced; the notifier is crash-safe (transport failures are logged as errors rather than aborting the logout).
- **Dead OpenAPI endpoints removed** (A1/M0): `/api/orgs*`, `/oauth2/device/verify`, and the old `/oauth2/mfa/*` paths removed from the spec — these endpoints no longer existed server-side (calls had been 404ing all along); the spec merely returns to the truth. With the oasdiff breaking-change gate in place, future HTTP-surface breaks require a MAJOR or an explicit exemption in `tools/openapi-governance/oasdiff-breaking-ignore.md`.

### Added

- **#43 resource-scope authorization model**: declarative `(path,method)→required_scopes` registry (`ResourceScopeRegistry`, 42 scope-gated routes) + scope inheritance (`impliedBy`, e.g. `admin` implies `roles:read/write`) + DB-driven admin role resolution + the `/api/admin/scopes/resources` discovery endpoint. Includes the V023 migration.
- **User-management CRUD completion** (A2, PR #52): `GET /api/admin/users` pagination (page/per_page) and filtering (q/role/locked); `createUser` (username vs email UNIQUE conflict distinction → 409; role persistence result reporting roles_assigned / roles_failed); `updateUser` extended (org_id supports integer assignment and null clearing); `deleteUser` soft delete (V024 `deleted_at` migration, revokes existing tokens on delete and reports `tokens_revoked`; last-active-admin protection → 409).
- **OIDC back-channel logout backend** (B1, PR #50): logout_token JWT builder + notifier wired into the logout flow (both `/oauth2/logout` and `end_session` trigger notification, #55); admin API and admin UI form configure `backchannel_logout_uri`; discovery advertises `backchannel_logout_supported`; validator unit tests D1–D6 + admin/discovery endpoint tests.
- **OpenAPI spec governance M0** (A1, PR #63): three-layer endpoint reconciliation (routes 82 = docs 80 = yaml 78, modulo a two-entry exception list); P0 schemas completed from controller-verified contracts (token/introspect/revoke/login form and JSON request bodies, RFC 6749 and application dual error shapes, discovery/JWKS full fields); `info.version` linked to `cmake/Version.cmake` (1.2.0). CI gains the three-layer consistency gate (`tools/openapi-governance/check_spec_governance.py`) + the oasdiff breaking-change gate (PR vs master, v1.29.1 pinned). Acceptance: the generated Python client actually calls token / introspect / discovery, all passing (client-SDK work unblocked).
- **benchmark M2–M4** (PR #48): S3–S6 scenarios (introspect / revoke / userinfo / discovery stateless endpoints); `config.bench.json` centralized config; 40 result JSONs + SUMMARY.md load-bearing verdicts — SDK-in-memory methodology measured **2.5 MB peak WS** (far better than the 50–120 MB claims), cold start observed at ~4 s, P99 1–4 ms at low concurrency, discovery ~86k QPS @ 8 vCPU.
- Endpoint tests (59 OAuth2 + 52 Admin) integrated into ctest and the platform CI gate.

### Fixed

- **#53–#60 user-management hardening batch** (PR #62): admin-surface input validation, error-code semantics, and concurrency behavior fixes; #59 frontend fix sending JSON null to clear org_id.
- Social-login default role grant was never actually persisted (role_id unset) — PR #62 review fix.
- `createUser`/`updateUser` UNIQUE conflicts return 409 instead of 500 (PR #52).
- benchmark: container resolution switched to `docker ps` (compose ps output drift); PR #48 review fixes (config swap detection, observer timing); in-memory verdict methodology fix (wrong metric, not a miss).
- Test infra: endpoint wrapper `pkill -f` self-kill bug, ctest pipe-inheritance hang, exit 77 skip when the server is unavailable, CI psql fallback grabbing an arbitrary postgres container.

### Changed

- The `IOAuthHttpClient` family is freed from `WITH_SOCIAL` conditional compilation (core paths such as backchannel logout need the HTTP client, decoupled from the social-login compile switch).
- refactor-baseline endpoint-signature baseline regenerated (78 lines, converging master's standing drift).

### Documentation & CI

- Productization-evolution docs synced (A2/B1/A1 delivery status, benchmark verdicts, issues #53–60 design and implementation plans); the openapi-update skill's four mirrors updated to the governance-gate workflow; CI adds the openapi-governance workflow. api-diff baseline re-ratified per SOP for #43 internal-engine and ORM regeneration drift.

## [1.1.0] - 2026-08-12

First formal release since v1.0.0. Spans 842 commits, including complete
OIDC Core support, OAuth/OIDC compliance-audit fixes (all 31 findings
remediated), the Redis cache layer, WebAuthn/Passkey, the admin console,
and the SDK library-layer restructuring.

### ⚠️ Breaking (security hardening)

> The following changes tighten previously lax (and mostly spec-violating)
> behavior. Per the versioning policy
> ([§3](docs/contribute/versioning-and-release.md#3-the-security-hardening-gray-zone--an-explicit-trade-off-statement))
> they proceed within a MINOR, not a MAJOR. Downstreams relying on the old
> lax behavior must migrate accordingly.

- **PKCE enforced for PUBLIC clients** (RFC 9700 §2.1.1): `require_pkce_for_public` defaults to `true` (set explicitly in `config.json` / `config.dev.json` / `config.ci.json` / `config.prod.json`). Migration: PUBLIC clients must send `code_challenge` / `code_verifier`.
- **redirect_uri forced to https** (RFC 8252 §7.3): exemptions are only the `http://127.0.0.1` and `http://[::1]` loopback IP literals (any port; `localhost` is **not** exempt). New config switch `auth.allow_http_redirect_uri` (on for dev / off for prod). Migration: change `localhost` redirect_uris in seeds/tests to `127.0.0.1`.
- **refresh_token grant enforces client authentication** (RFC 6749 §3.2.1/§6): a CONFIDENTIAL client with a missing or wrong `client_secret` gets 401 `invalid_client` (with `WWW-Authenticate: Basic`). Migration: CONFIDENTIAL clients must include client credentials in refresh requests.
- **token_endpoint_auth_method persisted and enforced**: the column added to `oauth2_clients`. token/introspect/revoke enforce the declared method (`client_secret_basic` accepts only the Basic header / `client_secret_post` only the body / `none` rejects any secret). NULL keeps the old lenient Basic→body fallback. Migration: assigning explicitly via the admin surface is recommended.
- **userinfo requires the openid scope**: an access token whose scope lacks openid returns 403 + `WWW-Authenticate: Bearer error="insufficient_scope"`; M2M tokens (subject `client:*`) are rejected outright.
- **Minimum-path required-scope enforcement**: `/oauth2/userinfo`→`openid`, `/api/me` and `/api/me/*`→`profile`, `/api/admin/*`→`admin` scope.
- **The standalone Redis storage mode is formally deprecated**: LOG_ERROR at startup makes it explicit; in that mode the refresh_token grant returns `unsupported_grant_type`. The target architecture is Postgres storage + Redis cache. See `docs/operate/configuration-guide.md` §3.
- **Internal rate limiting**: `/oauth2/token`, `/oauth2/introspect`, `/oauth2/revoke`, and device_code polling share a sliding-window limiter (default 30 failures/60 s, configurable via `custom_config["auth"]["rate_limit"]`); over the threshold → 429 + `Retry-After`. Failures only; successes reset the window.
- **SDK namespace restructuring**: `oauth2::*` → `authforge::drogon::*`, `common::*` → `authforge::common::*`. Flat `#include <oauth2/Foo.h>` paths are no longer valid; use semantic subdirectory paths (e.g. `<authforge/drogon/plugin/OAuth2Plugin.h>`). SDK consumers must update include paths and namespace qualifiers.

### Added

#### OAuth2 / OIDC protocol

- **OpenID Connect Core**: `id_token` issuance (HS256/RS256), `/.well-known/openid-configuration` discovery, the JWKS endpoint, `userinfo`, and full `prompt` / `max_age` / `auth_time` / `acr` / `amr` support.
- **RP-Initiated Logout** (OIDC): `/oauth2/end_session` (GET+POST), validating `id_token_hint` and `post_logout_redirect_uri`, echoing `state`.
- **Backchannel Logout** (OIDC): `backchannel_logout` support.
- **Device Authorization Grant** (RFC 8628): `/oauth2/device_authorization` endpoint, authentication branching by client_type; too-fast polling returns `slow_down` (interval increases by 5 s and is persisted).
- **Dynamic Client Registration** (RFC 7591): via `/api/admin/clients/*` (admin-only).
- **Client Credentials Grant** (M2M): machine-to-machine token issuance.
- **Token Introspection** (RFC 7662): `/oauth2/introspect`.
- **Token Revocation** (RFC 7009): `/oauth2/revoke`, including refresh-token revocation.
- **Authorization Server Metadata** (RFC 8414): `/.well-known/oauth-authorization-server`.

#### Identity & security features

- **Password hashing upgrade**: PBKDF2-SHA256 (replacing the old SHA-256).
- **Subject UUID**: `public_sub` replaces the auto-increment ID as the external subject.
- **Refresh token reuse detection**: family cascade invalidation (a stolen token reused → the whole family invalidated).
- **Authorization-code atomic consumption** + transactional token-pair persistence.
- **MFA / TOTP** (RFC 6238) and **WebAuthn / Passkey** support.
- **Account lockout**: progressive backoff.
- **Email verification** + **password reset** flows.
- **Structured audit logs** (`audit_logs` table + AuditService).
- **Multi-tenant foundation** (organizations).
- **SMTP mail service** (163 / Gmail / SendGrid generic).
- **Social login**: GitHub, Google.

#### Infrastructure

- **Redis L2 cache**: write-through decorators for client lookup and token caching (Postgres storage + Redis cache architecture), with admin-mutation invalidation.
- **SchemaManager**: numbered automatic migrations (`V0NN_*.sql`, 22 of them), single-transaction execution.
- **Multi-platform CI/CD**: Linux (Ubuntu 22.04), Windows (MSVC 2022), macOS (ARM64).
- **Tag-driven release pipeline**: SDK tarballs + multi-architecture images (amd64/arm64) + cosign keyless signing + SPDX SBOM + git-cliff release notes.
- **API surface guard**: the `api-diff` tool enforces SemVer for SDK public headers in CI (breaking requires a major).
- **arch-guard**: domain-layer boundary checks (no drogon headers included, etc.).
- **Helm chart** + production Compose hardening + versioned migration runner.
- **HTTP performance benchmark facility**.

#### Frontend

- **Production-grade SPA**: AuthForge user frontend (Vue), with PKCE, token lifecycle, the consent page, and email-first login/registration.
- **Admin console**: full management pages for clients/users/scopes/tokens/audit, with the OIDC signing-key view.
- **Design system rework**: a unified UI design language.

#### SDK library restructuring

- Layered library structure: `libs/common` (domain shared kernel), `libs/oauth2`, `libs/identity`, `libs/storage-{postgres,redis,memory}`, `libs/drogon` (adapter).
- Domain services decoupled from Drogon via ports (`ICryptoProvider` / `IUuidGenerator` / `IClock` / `ILogger` / `IAuditSink`), consumable outside a Drogon host.
- SDK packaging: `find_package(authforge-*)` source integration + install-consumption smoke test.

### Fixed

#### OAuth/OIDC compliance audit (31 findings, all remediated)

- **F-002**: the `client_secret` hashing write path unified to salted lowercase SHA-256, matching the verification path (the write path had used unsalted uppercase SHA-256, so clients created via dynamic registration/the admin surface could never authenticate).
- **F-004**: the Redis backend's `client_secret` comparison switched to constant-time; all three backends unified on `constantTimeMemcmp`; removed the LOG_DEBUG that leaked comparison results.
- **F-016**: issuer consistency fixed — access tokens now embed the configured issuer at issuance (previously never written); the three backends' hardcoded `https://oauth.example.com` removed; introspect `iss` and discovery `issuer` byte-identical (OIDC Discovery §3).
- **F-007**: authorization-endpoint errors split per RFC 6749 §4.1.2.1 — unknown client_id / invalid redirect_uri return 4xx directly; other errors 302-redirect with state echoed.
- **F-006**: resource endpoints send the RFC 6750 §3 `WWW-Authenticate: Bearer ... error="invalid_token"` challenge on 401.
- **F-008/F-009/F-013**: the token endpoint's validation gate emits the RFC 6749 §5.2 `error: invalid_request` envelope; redirect_uri strictly matched at authorization_code redemption; the authorize end validates `code_challenge_method ∈ {plain, S256}`.
- **F-019/F-020**: token/introspect/revoke success responses add `Cache-Control: no-store` (RFC 6749 §5.1 / RFC 7009 §2.2.1); terminal-authorize redirects urlEncode `state`/`code`.
- **Full audit report**: see
  `docs/adr/oauth-oidc-compliance-audit.md`.

#### Other fixes

- **Linux teardown crash**: `OAuth2CleanupService`'s destructor accessed a destroyed event loop → added a `stopped_` flag preventing repeated cleanup; clean exit without `std::_Exit(0)`.
- **refresh-token revoke was a no-op** (C3): the revoke path compared the token without hashing it, making RFC 7009 §2.1 revocation ineffective — fixed to hash before comparing.
- **SchemaManager single transaction**: the full migration pass executes inside a single transaction (#46).
- **keepalive logout revoke** (C5): keep-alive sessions correctly trigger token revocation on logout.
- **OpenAPI docs**: multiple fixes including the security field shape and PKCE/MFA/revoke parameter registration.
- **MSVC `/WX`**: resolved the `sharedCb` variable shadowing (C4458) that broke Windows CI.

### Changed

- **Drogon**: v1.9.10 → v1.9.13. `drogon_ctl`-generated ORM validation code switched to `std::wstring_convert<std::codecvt_utf8_utf16<...>>` (`orm_compat.h` handles the C++20 deprecation).
- **Dependency management**: introduced Conan (`conanfile.py` + `conan.lock`) for C++ dependency management.
- **Rate limiting**: migrated to the Drogon Hodor plugin (token bucket), removing the Redis dependency.
- **Token endpoint error responses**: unified to the RFC 6749 §5.2 OAuth2 error envelope (previously the application-internal envelope).
- **DB schema**: 14 new migrations (V009–V022), including refresh_token_family, mfa_support, account_lockout, device_codes, backchannel_logout, token_partitioning_prep, multi_tenant, webauthn, etc.

### Security

- OAuth/OIDC compliance audit: all 31 findings remediated (3 batches).
- Security hardening: SQL injection / XSS / command injection protection, CORS / CSP / HSTS headers, token revocation, account lockout.
- Constant-time `client_secret` comparison across all three storage backends.

---

## [1.0.0] - 2026-01-29

First formal release. OAuth2.0 Authorization Code Grant, access/refresh
tokens, client management, user authentication, the Drogon plugin
architecture, a Vue frontend, PostgreSQL/Redis persistence, RBAC, audit
logs, and WeChat login.

### Added

- OAuth2.0 Authorization Code Grant flow; access token and refresh token support.
- Client registration and management; user authentication (username/password).
- Drogon plugin architecture; controller-based HTTP endpoints; filter middleware; JSON configuration.
- PostgreSQL persistence (ORM migrations) and Redis persistence; synchronous writes; SHA-256 hashing.
- Atomic consumption operations; client secret hash verification; PostgreSQL transaction support.
- User account system; ORM migrations; UUID salt support.
- Vue.js SPA frontend; OAuth2 login flow; protected API access; user profile display.
- WeChat Open Platform API with QR-code login.
- RBAC permission-system foundation; Prometheus metrics; structured audit logs.
- Unit tests, integration tests (Redis/PostgreSQL), E2E integration tests, direct controller tests.

### Security

- Basic authentication (username/password); client secret hashing (SHA-256); CORS configuration.
- SQL injection protection; input validation and sanitization.

---

[1.0.1]: https://github.com/voidvec/fulla/compare/v1.0.0...v1.0.1
[Unreleased]: https://github.com/voidvec/fulla/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/voidvec/fulla/compare/v1.0.1...v1.1.0
[1.0.0]: https://github.com/voidvec/fulla/releases/tag/v1.0.0
