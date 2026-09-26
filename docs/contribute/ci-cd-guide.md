# CI/CD Pipeline Guide (CI/CD Guide)

This document describes the project's continuous integration and continuous delivery (CI/CD) mechanism, built on **GitHub Actions**.

---

## 1. Pipeline Overview

The CI configuration lives in `.github/workflows/ci.yml` and consists of three fail-fast, chained jobs (including a reusable-workflow matrix):

```
Push/PR to master (and workflow_dispatch)
        │
        ├── FAST gate
        │     ├── static-checks (ubuntu-24.04) — source-level guards:
        │     │     arch-guard / double-move capture guard (#183) /
        │     │     migration-check / api-diff /
        │     │     test naming / manage-script parity / OpenAPI checks /
        │     │     OpenAPI governance gate (three-layer consistency + version sync)
        │     └── frontend (_frontend.yml) — vitest unit/property tests, ESLint,
        │           production builds + bundle-size budget gate (#159),
        │           Playwright e2e, and the frontend Docker image build smoke
        │           (#160, path-filtered to frontends/** + deploy/docker/**)
        │
        ├── openapi-governance (openapi-governance.yml, PR-triggered) —
        │     oasdiff breaking-change gate (base vs PR openapi.yaml;
        │     exemption list tools/openapi-governance/oasdiff-breaking-ignore.md)
        │
        ├── MAIN gate
        │     └── build-test (_build-test.yml × {linux, windows, macos} matrix)
        │           ├── install system dependencies / Conan
        │           ├── configure and build (Conan + cmake --preset, with cache)
        │           ├── [linux] start Postgres/Redis containers and wait until ready
        │           ├── [linux] initialize the database schema
        │           ├── run ctest + release naming gate
        │           └── [on failure] upload test-log artifacts
        │
        └── RELEASE gate
              └── sdk-smoke (_sdk-smoke.yml) — full-stack find_package smoke test
```

---

## 2. Triggers

```yaml
on:
  push:
    branches: ["master"]
  pull_request:
    branches: ["master"]
  workflow_dispatch:

concurrency:
  group: ci-${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true
```

- **Push to master**: every merge to master automatically triggers the full check suite.
- **Pull Request**: triggered automatically on PR creation/update, serving as a pre-merge gate check.
- **workflow_dispatch**: manual triggering is supported.
- **Concurrency control**: a new run for the same branch cancels any in-progress older run.

---

## 3. Frontend Gate (`_frontend.yml`)

The frontend gate runs for every CI run (it is part of the FAST gate, so it fails cheap and early). One Node job plus one Docker job:

| Job | What it does |
|---|---|
| `frontend` | UI-kit byte-sync gate → vitest unit/property tests (both apps) → ESLint → production builds (`tsc && vite build`) → **bundle-size budget gate** (`scripts/check-frontend-size.mjs`, #159) → Playwright e2e (both apps, API-mocked) |
| `frontend-image-smoke` (#160) | Path-filtered (`frontends/**`, `deploy/docker/**`, this workflow) `docker build` of **both frontend images with `push: false`** — the admin image (`frontends/admin/Dockerfile`) and the user frontend stage of `deploy/docker/Dockerfile` (`target: frontend-runtime`) — mirroring exactly what `release.yml` builds, so a context/lockfile/base-image regression fails the PR instead of the release. GHA layer cache keeps warm runs near the npm-ci cost. |

Budget baselines live in `scripts/check-frontend-size.mjs` (raw JS bytes: total + entry chunk per app, +10% headroom). Raise a baseline only with written justification in the PR — the entry-chunk cap doubles as the i18n AOT guard: the vue-i18n message compiler re-entering the bundle costs ~90 KB on the main chunk and trips the gate mechanically.

## 4. Core Job in Depth: `build-test`

`build-test` is a reusable workflow (`_build-test.yml`) invoked by `ci.yml` as a `{linux, windows, macos}` matrix. All three platforms run the same Conan + `cmake --preset` build and CTest suite; the database is enabled via matrix inputs only where needed.

### 4.1 Service Containers (linux matrix leg only)

In the Linux matrix leg, CI starts Postgres and Redis in Docker containers and confirms readiness with real queries (not just `pg_isready`):

| Service | Image | Port | Password |
|---|---|---|---|
| PostgreSQL | `postgres:17-alpine` | `5432` | `123456` |
| Redis | `redis:7-alpine` | `6379` | None (simplified CI configuration)|

> The PostgreSQL image matches the deploy default (`postgres:17-alpine`, since 2026-08-18) — CI therefore covers
> the major version actually deployed (including how migrations such as V025 partitioning / V026 behave on 17).

> **WARNING**: Redis runs without a password in CI, so the test configuration overrides it with the environment variable `FULLA_REDIS_PASSWORD=""`. The Windows/macOS matrix legs use `use_database=false` and fall back to the in-memory storage configuration (`config.ci.json`).

### 4.2 Build Cache Strategy

To speed up CI builds, Conan dependencies are cached:

| Cache | Cache key | Contents |
|---|---|---|
| **Conan dependency cache** | `conan-{OS}-v1-cpp17-{conanfile.py + conan.lock hash}` | The `~/.conan2` directory (third-party dependencies, including Drogon)|

A cold build takes roughly **15-20 minutes**; with a cache hit this drops to **3-5 minutes**.

### 4.3 Database Initialization

Before testing, migration scripts initialize the database:

```bash
# Run all migration files in order
for f in apps/server/migrations/V*.sql; do
    psql -h localhost -U fulla_user -d fulla_db -f "$f"
done

# Load seed data (dev/test environments)
for f in apps/server/seed/*.sql; do
    psql -h localhost -U fulla_user -d fulla_db -f "$f"
done
```

> **Note**: the legacy `sql/001_*.sql` through `sql/004_*.sql` files are deprecated and removed; all schema definitions are now managed centrally under `apps/server/migrations/`.

### 4.4 Test Execution

```bash
ctest -V -C Release --output-on-failure --timeout 120
```

- `-V` : verbose output
- `--output-on-failure` : print test stdout on failure
- `--timeout 120` : each test gets at most 2 minutes

### 4.5 Failure Log Upload

When tests fail, CI automatically packages and uploads the following as artifacts (retained for 7 days):

- `build/Testing/` — CTest test reports
- `apps/server/logs/` — application runtime logs

---

## 5. Image Build and Signing

The CI pipeline itself does not build Docker images. Multi-arch container image builds, GHCR pushes, cosign signing, and syft SBOMs are handled by `release.yml` when a SemVer tag (`vX.Y.Z`) is pushed. See [Releases & Supply Chain Security](https://github.com/voidvec/fulla#releases--supply-chain-security).

---

## 6. Reproducing the CI Environment Locally

To simulate CI behavior locally:

```powershell
# 1. Start the infrastructure (CI uses service containers; locally, use Docker)
docker run -d -p 5432:5432 -e POSTGRES_USER=fulla_user -e POSTGRES_PASSWORD=123456 -e POSTGRES_DB=fulla_db postgres:17-alpine
docker run -d -p 6379:6379 redis:7-alpine

# 2. Initialize the database
$env:PGPASSWORD = "123456"
Get-ChildItem "apps\server\migrations\V*.sql" | Sort-Object Name | ForEach-Object {
    psql -h localhost -U fulla_user -d fulla_db -f $_.FullName
}
Get-ChildItem "apps\server\seed\*.sql" | ForEach-Object {
    psql -h localhost -U fulla_user -d fulla_db -f $_.FullName
}

# 3. Build and run tests (build.bat uses Conan + cmake --preset; Release lands in build/windows-msvc)
.\scripts\backend\build.bat -release
cd build\windows-msvc
$env:FULLA_REDIS_PASSWORD = ""
ctest -V -C Release --output-on-failure
```

---

## 7. Multi-Platform Matrix

Multi-platform CI has been consolidated into the `build-test` job in `ci.yml`; an `include` matrix runs all three platforms on the same reusable workflow (`_build-test.yml`).

### Quick Reference

- **Workflow File:** `.github/workflows/ci.yml` (invokes `_build-test.yml`)
- **Platforms:** Linux (ubuntu-24.04), Windows (windows-2022), macOS (macos-14)
- **Trigger:** Push to master, pull requests, manual workflow dispatch
- **Runtime:** ~15-20 minutes cold cache, ~3-5 minutes warm cache per platform

### Platform-Specific Features

Per-platform differences are expressed entirely through matrix inputs (no copy-pasted pipelines):

- **Linux:** system dependencies installed via apt; PostgreSQL/Redis run in Docker containers; performs database initialization and the release naming gate
- **Windows:** Conan dependency management with the MSVC 2022 compiler; in-memory storage configuration (`use_ci_config`), no external DB
- **macOS:** Homebrew (`brew update` only); arm64 builds (`-s arch=armv8`, runner is `macos-14`); in-memory storage configuration

---
