# Fulla — High-Performance Open-Source IAM Core (C++17)

[中文文档](README.zh-CN.md)

![CI](https://github.com/voidvec/fulla/actions/workflows/ci.yml/badge.svg)
![Security](https://github.com/voidvec/fulla/actions/workflows/security.yml/badge.svg)
[![Release](https://img.shields.io/github/v/release/voidvec/fulla)](https://github.com/voidvec/fulla/releases/latest)
[![PyPI](https://img.shields.io/pypi/v/fulla-oauth2.svg)](https://pypi.org/project/fulla-oauth2/)
[![Go Reference](https://pkg.go.dev/badge/github.com/voidvec/fulla/clients/go.svg)](https://pkg.go.dev/github.com/voidvec/fulla/clients/go)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)
![Conan](https://img.shields.io/badge/Conan-2.x-6699CB.svg)
![License](https://img.shields.io/badge/license-AGPL_v3-blue.svg)
[![Benchmark](https://img.shields.io/badge/benchmark-5%2F5%20scenarios%20lead-brightgreen)](benchmarks/competitors/results/COMPARISON.md)

Fulla is a high-performance **open-source identity & access management (IAM) core** built in
C++17: a production-grade OAuth2.0/OIDC authorization server (RFC 6749/7662/7009/8414) with
full support for user authentication, MFA, WebAuthn, RBAC and multi-tenancy — usable as a
**ready-to-run product** (Docker/Helm) or as an **embeddable C++ SDK**
(`find_package(fulla-*)`). Includes admin console, user-facing frontend, and a comprehensive
test suite.

> **Roadmap — open core:** the open-source core is the base for optional commercial
> enhancement modules (enterprise integration & support offerings, planned). Everything
> needed to run a complete IAM is and stays open source under AGPL-3.0.

---

## Capability Map

What the product does, by domain. Deep-dive links go to the guides under `docs/`
(session management, token lifecycle and multi-tenancy deep-dives are on the docs
roadmap).

| Domain | Capabilities | Deep dive |
|--------|--------------|-----------|
| **Authentication** | Login/registration, email verification, password reset, TOTP MFA, WebAuthn (FIDO2), Google/WeChat social login, progressive account lockout | [Security Architecture](docs/architecture/security-architecture.md) |
| **Authorization (OAuth2/OIDC)** | Auth-code + PKCE, client-credentials, refresh rotation, device flow, dynamic client registration, user consent, introspection, revocation, OIDC discovery/JWKS/UserInfo, end-session with front/back-channel logout | [Architecture Overview](docs/architecture/architecture-overview.md) |
| **Access control** | RBAC (built-in admin/user + custom roles), granular scopes, DB-driven resource-scope registry, triple check (client restriction + role + consent) | [RBAC Guide](docs/domains/rbac-guide.md) |
| **Token lifecycle** | Issuance, TTL-bounded retention, family-based refresh rotation, revocation by token/client/user, cache-aside with delayed double-delete invalidation | — |
| **Multi-tenancy** | Organizations, org-scoped clients and users, tenant-aware administration | — |
| **Observability** | Prometheus metrics, structured audit log (login/token/password events), health probes (live/ready) | [Observability](docs/operate/observability.md) |
| **Operations** | Docker Compose / Helm deploys, cosign-signed multi-arch images, SBOMs, config-file + env-driven configuration | [Production Deployment](docs/operate/deployment.md) |

## Module Map

The 8 CMake packages behind the [SDK layering](#sdk-layering) diagram — what each owns
and where its public headers live (`libs/<name>/include/fulla/…`):

| Package | Owns | Notes |
|---------|------|-------|
| `fulla::common` | Shared kernel: value objects, Result, error catalog, ports (clock, crypto, metrics, email, audit) | Zero Drogon dependency |
| `fulla::oauth2` | OAuth2/OIDC engine: grant flows, token service, PKCE, JWK manager, scope decision engine | Protocol core, storage-agnostic |
| `fulla::identity` | Authentication domain: users, sessions, MFA, WebAuthn, social accounts, role provider | Optional at build time |
| `fulla::drogon` | Drogon adapters: HTTP controllers, filters, views, OAuth2 plugin, admin services | What a server host links |
| `fulla::storage::memory` | In-memory repositories (tests, embedded single-process use) | Zero external deps |
| `fulla::storage::postgres` | PostgreSQL repositories + Drogon ORM models, schema manager | PostgreSQL 14+ |
| `fulla::storage::redis` | Redis cache-aside layer with delayed double-delete invalidation | Wraps any repository tier |
| `fulla::storage` concepts | `IClientRepository` / `IGrantRepository` / `ITokenRepository` / `IConsentRepository` contracts | Verified cross-tier by the contract test suite |

Feature surface shrinks via Conan/CMake options (`with_identity` / `with_social` /
`with_webauthn`) so SDK consumers only pull what they use.

---

## Architecture

```
fulla/
├── apps/server/        # Authorization server backend (Drogon C++ framework)
├── libs/               # SDK library packages (fulla::common/oauth2/identity/storage-*/drogon)
├── frontends/admin/    # Admin console frontend (Vue 3 + TailwindCSS)
├── frontends/user/     # User-facing frontend (Vue 3 + Pinia + TailwindCSS)
├── examples/           # SDK consumer examples (find_package smoke hosts)
├── deploy/             # Docker Compose, Helm chart, nginx, observability
├── tests/              # Backend test suite (unit / integration / contract)
├── scripts/            # Build, test, and operations scripts
└── docs/               # Project documentation
```

### SDK Layering

The backend is split into 8 CMake packages with an enforced dependency direction (Domain layer never depends on Drogon; verified by `tools/arch-guard` in CI). Arrows read "depends on":

```mermaid
graph TD
    server["fulla-server<br/>(apps/server)"] --> drogon
    drogon["fulla::drogon<br/>plugin · controllers · filters · views"] --> oauth2
    drogon --> identity
    drogon --> memory
    drogon --> redis
    drogon --> postgres
    memory["fulla::storage::memory"] --> oauth2
    redis["fulla::storage::redis"] --> oauth2
    postgres["fulla::storage::postgres<br/>(ORM models)"] --> identity
    oauth2["fulla::oauth2<br/>OAuth2/OIDC engine"] --> common
    identity["fulla::identity<br/>auth · MFA · WebAuthn · RBAC"] --> common
    common["fulla::common<br/>shared kernel · ports"]
```

Optional feature areas are gated by Conan/CMake options (`with_identity` / `with_social` / `with_webauthn`) so SDK consumers can shrink the dependency surface.

### Tech Stack

| Layer | Technology |
|-------|------------|
| Backend Framework | Drogon (C++17) |
| Database | PostgreSQL 14+ |
| Cache | Redis 7+ |
| Admin Console | Vue 3 + Vite + Pinia + TailwindCSS |
| User Frontend | Vue 3 + Vite |
| Testing | CTest (C++) + Playwright (E2E) + PowerShell (API) |
| Monitoring | Prometheus + Audit Logging |
| Deployment | Docker Compose / Nginx |

---

## Features

### OAuth2/OIDC Core Protocols

| Feature | Standard | Endpoint |
|---------|----------|----------|
| Authorization Code + PKCE | RFC 6749 / RFC 7636 | `/oauth2/authorize`, `/oauth2/login`, `/oauth2/token` |
| Client Credentials | RFC 6749 | `/oauth2/token` (grant_type=client_credentials) |
| Token Refresh | RFC 6749 | `/oauth2/token` (grant_type=refresh_token) |
| Token Introspection | RFC 7662 | `/oauth2/introspect` |
| Token Revocation | RFC 7009 | `/oauth2/revoke` |
| OIDC Discovery | RFC 8414 | `/.well-known/openid-configuration` |
| JWKS | RFC 7517 | `/.well-known/jwks.json` |
| UserInfo | OIDC Core | `/oauth2/userinfo` |
| User Consent | OAuth2 | `/oauth2/consent` |
| Device Authorization | RFC 8628 | `/oauth2/device_authorization` |
| Dynamic Client Registration | RFC 7591 | `/oauth2/register` |

### User Authentication & Security

| Feature | Endpoint |
|---------|----------|
| User Registration | `POST /api/register` |
| Password Reset | `/api/password-reset/request`, `/api/password-reset/confirm` |
| Email Verification | `/api/verify-email`, `/api/verify-email/resend` |
| MFA (TOTP) | `/api/me/mfa/setup`, `/api/me/mfa/verify`, `/api/me/mfa/disable` |
| WebAuthn (FIDO2) | `/api/me/webauthn/register/*`, `/oauth2/webauthn/authenticate/*` |
| Google Login | `/api/google/login` |
| WeChat Login | `/api/wechat/login` |
| Account Lockout | Progressive lockout (5/10/15/20 failed attempts) |

### User Self-Service

| Feature | Endpoint |
|---------|----------|
| Profile | `GET /api/me` |
| Change Password | `PUT /api/me/password` |
| Authorized Apps | `GET/DELETE /api/me/authorized-apps` |
| Account Deletion | `DELETE /api/me` |

### Admin Console (frontends/admin)

| Module | Features |
|--------|----------|
| Dashboard | User count, app count, active tokens, failed login stats |
| App Management | Client CRUD, secret rotation, scope assignment, grant type config |
| User Management | User list/details, role assignment, disable/enable, lock status |
| Role Management | Role CRUD (protects built-in roles: admin/user) |
| Scope Management | Scope CRUD (protects built-in scopes: openid/profile/email/admin) |
| Token Management | Token listing, revocation by client/user, individual revocation |
| Organization Management | Multi-tenant organization CRUD |
| Audit Log | Paginated view, filter by event type/result |
| OIDC Keys | Signing key information |
| System Settings | Health monitoring |

### RBAC Permission System

- Role-based access control (admin / user / custom roles)
- URL pattern matching for permission checks (`/api/admin/.*` requires admin role)
- Triple-scope permission control (Client restriction + Role validation + Consent check)

### Observability

- Prometheus metrics export (`/metrics`)
- Structured audit logging (login, token issuance/revocation, password changes, etc.)
- Health check endpoints (`/health`, `/health/live`, `/health/ready`)

---

## Performance

Same-environment comparison against Keycloak 26.7.1, Ory Hydra v26.2.0, and Zitadel v4.17.1
(single idle host, serial same-session runs, each product on its officially recommended config,
identical PostgreSQL 17 backend, identical wrk staircase 2→128). Fulla runs its documented
bench profile (pool 64/64, cache on, `auto_batch`, `reuse_port`, opt-in LTO build, TTL=30
retention-bounded sessions). **All five comparison scenarios lead** (2026-08-23 refresh,
[full report + methodology](benchmarks/competitors/results/COMPARISON.md)):

| Scenario | Fulla | vs runner-up |
|---|---|---|
| discovery (`/.well-known/openid-configuration`) | 87,499 QPS | 2.1x Keycloak |
| client_credentials token issuance | 14,438 QPS | 2.6x Keycloak |
| token introspection | 22,458 QPS | 2.0x Ory Hydra |
| refresh_token rotation | 5,506 QPS | 1.9x Keycloak |
| userinfo | 49,302 QPS | 1.5x Keycloak |
| cold start (stack → first 200) | 1.26 s | 14.5x faster than Keycloak |

Honest qualifiers: measured on WSL2 8 vCPU / 16 GB (numbers are lower bounds, not bare-metal);
the "lightweight" story holds for the **embedded-SDK footprint (2.5 MB peak working set)**, not
the full container stack; per-segment P99 jitter is host-noise-dominated on this rig and is not
claimed as a differentiator (see the report's GC section).

### How to reproduce

```bash
# One command, four products, same-session serial (needs Docker + an idle host; ≈3 h end-to-end):
bash benchmarks/competitors/run-comparison.sh --fresh
# Regenerate the report from the committed result JSONs (no hand-typed numbers):
python3 benchmarks/reporting/gen-comparison.py
```

Methodology and fairness deviations (per-product config sources, what is and isn't aligned):
[competitor-benchmark-design.md](docs/benchmark/competitor-benchmark-design.md).
Fulla-side scenario details: [benchmarks/README.md](benchmarks/README.md).

---

## Quick Start

### Path A — Docker Compose (recommended for evaluation)

```bash
docker compose -f deploy/docker/docker-compose.yml up -d --build
```

- User Frontend: `http://localhost:8080`
- Admin Console: `http://localhost:8081`
- Backend API: `http://localhost:5555`

> **Dev-only credentials (#112):** this compose file hardcodes weak passwords
> (DB `123456`, Redis `redis_secret_pass`, vue-client `123456`, seeded
> `admin`/`admin`, stored as a PBKDF2 hash since #103) and the seed admin
> account exists for evaluation only.
> PostgreSQL (127.0.0.1:5433) and Redis (127.0.0.1:6380) are loopback-bound on
> purpose. For anything beyond local evaluation use
> [`docker-compose.prod.yml`](deploy/docker/docker-compose.prod.yml) with
> `.env.docker` — the production validator (`FULLA_ENV=production`) rejects
> these defaults at startup.

### Path B — Build from source

The canonical build is Conan 2 + CMake presets (identical to CI):

```bash
# 1. Resolve locked dependencies (writes toolchain into the preset's build dir)
conan install . --output-folder=build/linux-release --build=missing \
  -s build_type=Release -s compiler.cppstd=17

# 2. Configure + build
#    Presets: linux-release / windows-msvc / macos-arm64 (+ -debug / -asan / -tsan variants)
cmake --preset linux-release
cmake --build --preset linux-release

# 3. Run the backend test suite
ctest --test-dir build/linux-release --output-on-failure
```

`manage.ps1` (Windows) and `manage.sh` (Linux/macOS) wrap the same flow as convenience commands, e.g. `.\manage.ps1 build-backend`.

To run the full stack locally (backend requires PostgreSQL + Redis):

```powershell
# Backend
cd apps\server
..\..\build\windows-msvc\apps\server\Release\fulla-server.exe

# Admin console — http://localhost:5174/admin/
cd frontends\admin && npm install && npm run dev

# User frontend — http://localhost:5173
cd frontends\user && npm install && npm run dev
```

### Path C — Consume as an SDK

Embed Fulla into your own C++ host via `find_package` (SDK tarball from [Releases](https://github.com/voidvec/fulla/releases), or `cmake --install` from source):

```cmake
# Full stack: one package pulls the whole closure (engine + Drogon plugin/controllers)
find_package(fulla-drogon CONFIG REQUIRED)
target_link_libraries(my-host PRIVATE fulla::drogon)

# Or engine-only (no Drogon dependency):
find_package(fulla-oauth2 CONFIG REQUIRED)
find_package(fulla-storage-memory CONFIG REQUIRED)
target_link_libraries(my-engine PRIVATE fulla::oauth2 fulla::storage::memory)
```

> v1.x promises **source-level SemVer** for the public headers (`include/fulla/**`), enforced by an api-diff gate in CI — no binary ABI guarantee. Resolve third-party dependencies with the repository's `conanfile.py` + `conan.lock`. Details: [SDK Integration Guide](docs/sdk/sdk-integration-guide.md) · [SDK Runtime Contract](docs/sdk/sdk-runtime-contract.md); reference consumers: [`examples/full-stack-host`](examples/full-stack-host), [`examples/third-party-host`](examples/third-party-host) (both CI-verified).

### Path D — Client SDKs (Python / Go)

Non-C++ services talk to Fulla over its HTTP API with generated, typed clients plus a handwritten auth layer (token lifecycle is never templated):

```python
# Python (distribution fulla-oauth2, import fulla)
from fulla import m2m_client

client = m2m_client("http://localhost:5555", "backend-svc", "…", scopes=["tokens:read"])
```

```go
// Go (github.com/voidvec/fulla/clients/go)
client, _ := af.NewM2MClient(ctx, "http://localhost:5555", "backend-svc", "…", []string{"tokens:read"})
```

Both are generated from the single-source OpenAPI spec (`apps/server/openapi.yaml`) with a CI freshness gate. See [`clients/python`](clients/python) and [`clients/go`](clients/go). Package registry publishing (PyPI / Go module proxy) starts with the first tagged release shipping them.

### Default Credentials

| Username | Password | Role |
|----------|----------|------|
| admin | *(auto-generated on first boot — see the server log once, or set `FULLA_BOOTSTRAP_ADMIN_PASSWORD`)* | admin |

---

## Deployment

| Target | Entry point | Notes |
|--------|-------------|-------|
| Docker Compose (dev) | `deploy/docker/docker-compose.yml` | Full stack + PostgreSQL + Redis, single command |
| Docker Compose (prod) | `deploy/docker/docker-compose.prod.yml` | TLS/nginx, env-file driven secrets |
| Kubernetes (Helm) | `deploy/helm/fulla` | Chart with values-driven config; schema migration runs as a Helm hook Job |

```bash
helm install fulla deploy/helm/fulla -f my-values.yaml
```

Full walkthroughs: [Production Deployment Guide](docs/operate/deployment.md) · [Windows / Docker Desktop](docs/operate/deployment-windows-docker-desktop.md) · [Security Checklist](docs/architecture/security-architecture.md)

---

## Releases & Supply Chain Security

Releases are cut from SemVer tags (`vX.Y.Z`) by [`release.yml`](.github/workflows/release.yml):

- **SDK package** — `fulla-sdk-<ver>-linux-x86_64.tar.gz` (8 static libs + headers + CMake package configs) with `.sha256` checksum, attached to the GitHub Release.
- **Container images** — multi-arch (amd64 + arm64) on GHCR: `ghcr.io/voidvec/fulla-{backend,frontend,admin}:<ver>`.
- **Signatures** — image manifests are signed by digest with cosign (keyless, GitHub OIDC).
- **SBOMs** — SPDX JSON for each image and the source tree (syft), attached to the Release.

Verify before deploying:

```bash
# Image signature
cosign verify ghcr.io/voidvec/fulla-backend:<version> \
  --certificate-identity-regexp 'github.com/voidvec/.+/.github/workflows/release.yml' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com

# SDK tarball integrity
sha256sum -c fulla-sdk-<version>-linux-x86_64.tar.gz.sha256
```

---

## Testing

### Backend API Tests

```powershell
# Admin API full tests
.\scripts\backend\test-admin-endpoints.ps1

# OAuth2 core flow tests
.\scripts\backend\test-oauth2-endpoints.ps1
```

### Frontend E2E Tests

```powershell
cd frontends\admin
npx playwright test              # Full run
npx playwright test --ui         # UI mode for debugging
npx playwright test --headed     # Headed browser mode
```

### C++ Unit Tests

```powershell
cd build\windows-msvc
ctest --output-on-failure
```

### Test Coverage

| Test Type | Scope |
|-----------|-------|
| C++ Unit/Integration Tests (CTest) | SDK libraries, domain services, storage adapters |
| Admin API (PowerShell) | All Admin endpoints + Organization |
| OAuth2 Core (PowerShell) | Auth flows, token management, user services |
| Frontend E2E (Playwright) | Admin console and user frontend pages/interactions |

---

## API Documentation

- **OpenAPI Spec**: [openapi.yaml](apps/server/openapi.yaml)
- **Swagger UI**: `http://localhost:5555/docs/api` (requires Swagger UI static files)
- **E2E Testing Guide**: [Admin E2E Methodology](docs/contribute/admin-e2e-testing-guide.md)

---

## Documentation

**Read the docs at [fulla.dev](https://fulla.dev)** — the site is built straight from this repo's `docs/` tree.

**Evaluating** — [Architecture Overview](docs/architecture/architecture-overview.md) · [Security Architecture](docs/architecture/security-architecture.md) · [RBAC Guide](docs/domains/rbac-guide.md) · [Benchmarks](benchmarks/competitors/results/COMPARISON.md)

**Integrating** — [SDK Integration Guide](docs/sdk/sdk-integration-guide.md) · [SDK Runtime Contract](docs/sdk/sdk-runtime-contract.md) · [API Reference](docs/domains/api-reference.md) · [OIDC Guide](docs/domains/oidc-guide.md)

**Operating** — [Production Deployment](docs/operate/deployment.md) · [Configuration Guide](docs/operate/configuration-guide.md) · [Observability](docs/operate/observability.md) · [Account Lockout](docs/operate/account-lockout.md)

**Contributing** — [CONTRIBUTING.md](CONTRIBUTING.md) · [Testing Guide](docs/contribute/testing-guide.md) · [CI/CD Pipeline](docs/contribute/ci-cd-guide.md) · [Versioning & Release](docs/contribute/versioning-and-release.md)

Full index: [docs/README.md](docs/README.md)

---

## System Requirements

| Component | Minimum Version |
|-----------|-----------------|
| C++ Compiler | C++17 (MSVC 2019+ / GCC 9+ / Clang 10+) |
| CMake | 3.21+ |
| PostgreSQL | 14+ |
| Redis | 7+ |
| Node.js | 18+ |
| Docker | 24+ (optional) |

---

## Contributing & Security

- Contributions welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for build, test, and commit conventions.
- To report a vulnerability, follow [SECURITY.md](SECURITY.md) — please do **not** open a public issue.

---

## License

AGPL-3.0 — see [LICENSE](LICENSE). Prior releases (v1.0.0) remain MIT.

---

**Project Status**: Production Ready | **Version**: v1.2.0
