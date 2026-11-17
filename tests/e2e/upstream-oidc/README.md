# Upstream OIDC federation E2E (Keycloak)

P2-1's acceptance run: a **real** Keycloak instance as the OIDC-in upstream,
proving the registry-driven federation chain end to end —

```
register provider (admin CRUD)
  → authorize transaction (guard: Origin + binding cookie + one-time state)
  → Keycloak login form POST (real user, real credentials)
  → code on the registered redirect
  → POST /api/auth/oidc/{provider}/login (code + state)
  → first-party token pair
  → GET /oauth2/userinfo (sub/email resolved from Keycloak's userinfo
    through the registry claims mapping)
  → login page advertises the provider (type=oidc)
```

## Run locally (WSL / Linux + Docker)

```bash
# Prereqs: the build tree (fulla-server binary) and the two infra containers
docker run -d --name fulla-model-pg -e POSTGRES_USER=fulla_user \
  -e POSTGRES_PASSWORD=123456 -e POSTGRES_DB=fulla_db -p 55432:5432 postgres:17-alpine
docker run -d --name fulla-diag-redis -p 56379:6379 redis:7-alpine --requirepass 123456

bash tests/e2e/upstream-oidc/run-e2e.sh
```

Useful env knobs (all optional):

| Var | Default | Meaning |
|---|---|---|
| `FULLA_SERVER_BIN` | auto-detect `build/linux-release/...` | server binary under test |
| `KC_PORT` | `18080` | published Keycloak port |
| `KEYCLOAK_IMAGE` | `quay.io/keycloak/keycloak:26.7.1` | override for quay-blocked networks |
| `KEEP_STACK` | unset | keep containers + work dir after the run (debug) |
| `SKIP_PROVISION` | unset | realm already exists (dev loop) |

The script regenerates every upstream credential per run (`/dev/urandom`);
nothing credential-like is committed. The fulla database is dropped and
re-seeded each run, and the Keycloak container is removed afterwards.

## CI

`.github/workflows/interop.yml` runs this script on every push to master
(job `upstream-oidc-e2e`): it builds the server, starts the two infra
containers plus Keycloak, and executes the same script — one job, one
green/red signal for the whole federation chain.

## What this covers vs. the in-process suite

- The in-process integration tests (`UpstreamProviderAdminHttpTest`) pin the
  guard/CRUD/listing contracts **hermetically** (pinned `endpoint_overrides`,
  no live upstream).
- `OidcUpstreamAuthServiceTest` pins the protocol with fake transports.
- THIS run is the only place a **real** third-party IdP executes the whole
  chain (discovery, browser login form, code issuance, token exchange,
  userinfo claims) — it is the regression tripwire for upstream-facing
  regressions the fakes cannot see (e.g. the dual `content-type` header bug
  this E2E caught on its first run).
