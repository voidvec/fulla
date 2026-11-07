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

> `ci.yml` is only the **per-PR entry point**. `.github/workflows/` holds **13 workflow files** in total; the rest run on their own triggers (path-filtered PR gates, schedules, tags, or manual dispatch). Section 3 covers all of them.

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

## 3. Workflow Inventory (all 13 files under `.github/workflows/`)

Three of the 13 files are **reusable workflows** (`_build-test.yml`, `_frontend.yml`, `_sdk-smoke.yml`, all `on: workflow_call`) — they run only when `ci.yml` calls them. The other ten are independent top-level workflows with their own triggers and path filters.

Classification used below:

- **Gate** — no `continue-on-error`; a failure turns the run red, and for `pull_request`-triggered workflows blocks the merge.
- **Reference** — the step carries `continue-on-error: true`: it reports (step summary, annotations, artifacts) but cannot fail the run by itself.
- **Post-merge / release-only / manual** — never fires on a PR; runs on `master`/`release/**` pushes, tags, or `workflow_dispatch` only.

### 3.0 Summary table

| File | Trigger (+ path filter) | What it does | Classification |
|---|---|---|---|
| `ci.yml` | push master / PR master / dispatch (no paths) | FAST → MAIN → RELEASE gates: static checks, frontend, 3-platform build+test, SDK smoke | Gate |
| `_build-test.yml` | `workflow_call` (from `ci.yml`) | Conan + `cmake --preset` build, full ctest, named test gates, log artifacts | Gate |
| `_frontend.yml` | `workflow_call` (from `ci.yml`) | UI-kit sync, vitest, ESLint, tsc builds, bundle budget, Playwright e2e, frontend image smoke | Gate |
| `_sdk-smoke.yml` | `workflow_call` (from `ci.yml`) | Full-stack build + `SdkSmoke.FullStack` external `find_package` consumer | Gate |
| `openapi-governance.yml` | PR master / dispatch (paths: `apps/server/openapi.yaml`, `tools/openapi-governance/**`, self) | oasdiff breaking-change gate, base vs PR spec | Gate |
| `clients-sdk.yml` | PR master / push master / dispatch (paths: `clients/**`, `tools/clients/**`, `apps/server/openapi.yaml`, self) | Python/Go/TS clients: generated-tree drift gate + version linkage + unit tests | Gate |
| `security.yml` | push master / PR master / weekly cron (Mon 03:00 UTC) / dispatch (no paths) | Dependency EOL scan over `conan.lock` + secret/sensitive-file hygiene | Gate (cron leg: alert for newly-EOL'd deps) |
| `sanitizer.yml` | PR master / push master / nightly / dispatch (paths: `libs/**`, `apps/server/**`, `tests/**`, `cmake/**`, `CMakePresets.json`, self on PR) | `asan-ubsan`: full ctest under ASan+UBSan vs real Postgres+Redis; `tsan-nightly`: full ctest under TSan | `asan-ubsan`: Gate; `tsan-nightly`: nightly (cron added 2026-11-07) + dispatch |
| `coverage.yml` | PR master / push master / nightly / dispatch (paths: `libs/**`, `apps/server/**`, `tests/**`, `tools/coverage/**`, + script/cmake/self on PR) | gcov instrumented build, per-library coverage report, ratchet vs baseline | PR leg: advisory (60 min); **gate leg: suite completion + ratchet enforced** (push master + nightly, 150 min) |
| `website.yml` | push master / PR (any branch, paths: `docs/**`, `blog/**`, `website/**`, `scripts/gen-llms-txt.mjs`, self) / dispatch | llms.txt freshness gate + Docusaurus build (`onBrokenLinks: 'throw'`); Pages deploy on master/dispatch | Gate (build); deploy: master push / dispatch only |
| `interop.yml` | push master / push `release/**` / dispatch (no paths) | RP interop smoke: three RP libraries + nextjs-demo browser driver vs a real fulla stack | Post-merge / release-branch gate (deliberately off the PR hot path) |
| `release.yml` | push tag `vX.Y.Z` (strict SemVer) / dispatch (dry run) | Version check, SDK tarball, multi-arch images, cosign + SBOM, GitHub Release, PyPI | Release gate (tag only) |
| `npm-publish.yml` | dispatch only | `fulla-oauth2` npm package: verify, pack, publish (needs `NPM_TOKEN`) | Manual; never blocks a release |

### 3.1 `ci.yml` — CI

**Trigger**: push master / PR master / workflow_dispatch; concurrency cancels superseded runs per ref.

The single entry point that chains the three gates via `needs` (see the tree in §1):

- **`static-checks`** (ubuntu-24.04, 10 min) — source-only, no compilation: `arch_guard.py` (domain libs stay framework-agnostic), `double_move_guard.py` (#183 crash guard), `migration_check.py`, `api_diff.py` (SemVer over exported SDK headers vs `tools/api-diff/api-baseline.txt`), test naming validator, `drogon_macro_bool_check.py`, `manage-parity-check.sh`, the tracked-markdown link gate, OpenAPI spec validation + the spec governance gate (routes == docs == `openapi.yaml`, version sync).
- **`frontend`** — calls `_frontend.yml` (see §3.3 and §4).
- **`build-test`** — calls `_build-test.yml` as a `{linux, windows, macos}` matrix (`fail-fast: false`; each leg 60 min, `-DFULLA_WERROR=ON` everywhere; see §3.2 and §5).
- **`sdk-smoke`** — calls `_sdk-smoke.yml` after build-test (see §3.4).

**Compile caching (2026-11-07)**: every compiling workflow (`_build-test` Linux+macOS legs, `_coverage-run`, `sanitizer`, `_sdk-smoke`) builds through **ccache** (`CMAKE_{C,CXX}_COMPILER_LAUNCHER`, 2 GB cap, cached via `actions/cache` keyed on `conan.lock` + SHA with prefix fallbacks). Measured motivation: the linux leg spent ~29 of its ~33 min compiling ~360 first-party TUs with dependencies already cached. One cache directory safely serves the release/debug/asan/tsan/gcov variants — ccache keys on the preprocessed input, and the flag sets hash into distinct entries. Windows is excluded for now (MSVC + ccache on hosted runners is flaky). Each leg logs its `ccache -s` hit-rate after the build; expect cold-cache first runs and single-digit-minute warm rebuilds on PR iterations.

### 3.2 `_build-test.yml` — Reusable Build & Test (MAIN gate)

One parameterized job body shared by all three platforms (differences only via inputs). Steps: system deps / Conan install → Conan cache → configure + build → (linux) Postgres/Redis containers, readiness probes, schema + seed init → full ctest → named test gates (`run_named_test_gates`, linux+windows only): Error-Standardization Req 12.x tests, `Performance_P1_Benchmark_*` in-process cases, E2E and RedirectUri integration entries → upload test logs on failure (7-day retention). Detailed behavior: §5.

### 3.3 `_frontend.yml` — Reusable Frontend Quality Gate (FAST gate)

Two jobs: `frontend` (Node 20) and `frontend-image-smoke` (#160, Docker, path-filtered via `dorny/paths-filter` to `frontends/**` + `deploy/docker/**`). Detailed behavior: §4.

### 3.4 `_sdk-smoke.yml` — Reusable SDK Smoke (RELEASE gate)

Linux-only (single authoritative SDK-consumability proof), 60 min. Builds the full stack with `BUILD_EXAMPLES=ON` (build-tree package configs) and runs `ctest -L SdkSmoke`: `SdkSmoke.FullStack` uses `ctest --build-and-test` to configure, build, and run `examples/full-stack-host` as a real external `find_package` consumer. The smoke host runs with memory storage (`config/smoke.json`) — no database. Gate: yes.

### 3.5 `openapi-governance.yml` — OpenAPI Governance

**Trigger**: PR master (paths: `apps/server/openapi.yaml`, `tools/openapi-governance/**`, the workflow itself) + workflow_dispatch; 5 min; needs `fetch-depth: 0` to extract the base spec from `origin/master`.

Runs pinned **oasdiff v1.29.1** `breaking` between the base-branch spec and the PR spec; any breaking change (path removal, request-body tightening, response narrowing, ...) fails **unless** the change ships with a major version bump or is explicitly exempted with justification in `tools/openapi-governance/oasdiff-breaking-ignore.md`. Gate: yes. Reproducible locally with the same `oasdiff` command.

### 3.6 `clients-sdk.yml` — Client SDKs

**Trigger**: PR master / push master / dispatch, path-filtered to `clients/**`, `tools/clients/**`, `apps/server/openapi.yaml`, and the workflow itself — unrelated PRs are not affected. Kept outside `ci.yml`'s static-checks on purpose (needs Python + Go + Node toolchains and the pinned generators).

Three 15-min jobs:

- **`python`** — pinned `openapi-python-client==0.29.0`; **drift gate** (`regen_clients.py --check --python-only`: the committed generated tree must match `openapi.yaml`) + pytest unit tests (MockTransport; integration auto-skips).
- **`go`** — build + vet + gofmt; **drift gate** (`--go-only`) + `go test ./...` (httptest; integration behind `-tags integration`).
- **`ts`** — Node 22; **drift gate** (`--ts-only`) + **version linkage** (`--version-only`: `package.json` == `pyproject.toml` == `cmake/Version.cmake`) + `npm run check` (tsc emit + vitest red lines).

Gate: yes, all three jobs. Local equivalents are listed in the workflow header comment.

### 3.7 `security.yml` — Security

**Trigger**: push master / PR master / **weekly cron Mondays 03:00 UTC** / dispatch — the schedule exists so newly-EOL'd dependencies get flagged even when no commits land. Two 5-min jobs, both gates on push/PR:

- **`dependency-eol`** — `tools/security/dependency_eol_check.py` over `conan.lock` (policy floors: openssl >= 3.0, zlib >= 1.2.13, libcurl >= 8.4.0).
- **`secret-hygiene`** — `scripts/security-check.sh`: tracked `.env` files, hardcoded key patterns, `.gitignore` coverage, config permissions.

### 3.8 `sanitizer.yml` — Sanitizers

**Trigger**: PR master / push master / dispatch, path-filtered to `libs/**`, `apps/server/**`, `tests/**`, `cmake/**`, `CMakePresets.json` (the workflow file itself only on PR).

- **`asan-ubsan`** (gate, 60 min) — GCC Debug build via the `linux-asan-ubsan` preset (UBSan with `-fno-sanitize-recover` behavior via `halt_on_error=1`, ASan with leak detection), full ctest (900 s per test) against real Postgres 17 + Redis 7 containers, so integration suites are covered. Logs uploaded on failure. Triage policy: real defects are fixed in the same PR; third-party/system-call false positives get a documented suppression file under `tools/sanitizers/`.
- **`tsan-nightly`** (TSan, 90 min) — `if: schedule || dispatch`; TSan cannot compose with ASan/UBSan, hence a separate job running the **full** suite under the `linux-release-tsan` preset. Runs nightly via `schedule: 0 3 * * *` (the cron was added 2026-11-07 — until then the job was dispatch-only despite its name).

### 3.9 `coverage.yml` — Coverage

**Trigger**: PR master / push master / nightly schedule / dispatch, path-filtered to `libs/**`, `apps/server/**`, `tests/**`, `tools/coverage/**` (the PR leg additionally watches `scripts/measure_coverage.py`, `cmake/Coverage.cmake`, `CMakePresets.json`, and the workflows themselves). One reusable body (`_coverage-run.yml`) runs as **two legs**:

- **advisory** (PRs, 60 min): build with `FULLA_TEST_COVERAGE` instrumentation, best-effort ctest (`continue-on-error`), explicit `gcov -j -p` JSON pass (fails hard on zero `.gcda`/`.gcov.json.gz` — a false-green report is itself a failure), per-library summary, ratchet as reference-only. A coverage regression here is a signal to investigate, not a blocker.
- **gate** (push master + nightly 03:30 UTC + dispatch, 150 min): the same body with suite completion REQUIRED (ctest failures propagate) and the **ratchet as a hard gate** — a no-decrease regression fails within a day of merge.

Both legs share steps 2–5 above (gcov JSON pass hard-fails on empty data; `measure_coverage.py --ratchet tools/coverage/ratchet-baseline.json` compares each library against the committed baseline — see the [Testing Guide §7](testing-guide.md); raw gcov JSON + test logs uploaded as artifacts).

**Why two legs (measured 2026-11-07)**: the full 61k-assertion suite takes ~3.5 min at Release on the CI linux runner; gcov instrumentation slows the in-process server 10–20×, so the instrumented suite needs ~35–70 min of test wall time — it cannot fit a 60 min PR leg that also carries the ~29 min build. Suite sharding was evaluated and rejected: `OAuth2Tests` is one `add_test()` over the monolithic test binary, per-case `ctest -r` invocations would re-bootstrap the server for every case (only the repository-only Contract suite can afford that), and splitting the binary is a build-system refactor out of proportion for a 3.5-min-Release suite. A dedicated larger runner would work but is unnecessary once the ceiling is honestly sized (≈100 min fits the standard hosted runner; max 360).

### 3.10 `website.yml` — Website

**Trigger**: push master / PR on **any branch** (paths: `docs/**`, `blog/**`, `website/**`, `scripts/gen-llms-txt.mjs`, the workflow) / dispatch (manual deploy from a branch before merge). Concurrency group `pages` does **not** cancel in progress.

- **`build`** (gate) —
  1. **llms.txt freshness gate**: regenerate `website/static/llms.txt` + `llms-full.txt` from `docs/` via `scripts/gen-llms-txt.mjs` and `git diff --exit-code` them, so the committed AI-native artifacts can never drift stale;
  2. Docusaurus build (`npm ci && npm run build`) with **`onBrokenLinks: 'throw'`** in `website/docusaurus.config.js` — broken links fail the build, replacing manual link checking;
  3. upload the Pages artifact.
- **`deploy`** — only on master push or dispatch; publishes to GitHub Pages (requires Pages Source = GitHub Actions).

### 3.11 `interop.yml` — RP Interop Smoke

Per the workflow's own header: the **v1.5.0 P0 exit gate** ("three libraries, zero warnings"). Four drivers — three standard RP libraries (`openid-client` v6 / `authlib` / `go-oidc` + `x/oauth2`) drive the authorization-code + PKCE → token → userinfo → refresh → end-session chain **in strict mode** against a real fulla stack, plus the P1-5 **nextjs-demo Playwright smoke**, a fourth driver that drives a **real browser** through the first-party `fulla-oauth2` TS SDK (landing → fulla login form → callback with state+PKCE verified → profile/userinfo → protected API → RP-initiated logout) — a different recipe from the three json-mode drivers.

**Trigger**: push to `master` or `release/**` + workflow_dispatch. Deliberately **not** on the per-PR hot path (runtime cost); it is the post-merge/release-branch interop gate. Single job (45 min): build only the `fulla-server` target (linux-release), boot Postgres 17 + Redis 7, apply migrations + seeds, boot the server and poll `/health`, then run the four drivers in order (Node, Python, Go from inside `tests/interop/go`, then the demo: build `clients/ts`, `npm ci`, Playwright chromium, `next start -p 8080`, `npx playwright test`). On failure the server log's tail is printed. See `tests/interop/README.md`.

### 3.12 `release.yml` — Release

**Trigger**: push of a **strict SemVer tag** (`v1.2.3` pattern; suffixed tags such as `v1.0.0-something` intentionally do NOT release) + workflow_dispatch as a **dry run** (everything builds; nothing is pushed; manifest/release skipped). Concurrency never cancels a half-pushed release. Jobs:

- **`version-check`** — tag must equal `cmake/Version.cmake`; re-runs the OpenAPI spec governance check and the api-diff frozen-surface check (which also cross-checks cmake vs CMakeLists vs conanfile).
- **`sdk-package`** — Linux x86_64 SDK tarball (8 static libs + headers + CMake package configs via `cmake --install`), validated by configuring + building `examples/full-stack-host` against the **installed** prefix; sha256 attached.
- **`images`** — backend / frontend / admin images, amd64 + arm64, each on a **native** runner (no QEMU — the C++ build is impractical under emulation), pushed to GHCR as `<version>-<arch>` (tag runs only).
- **`manifest`** (tag only) — multi-arch manifests `<version>` + `latest` via imagetools, then cosign **keyless** signatures on the manifest digests and per-image SPDX SBOMs (syft).
- **`github-release`** (tag only) — Release notes from git-cliff (conventional commits) with a CHANGELOG fallback + spliced breaking-change entries + verification instructions; SDK tarball and SBOMs attached; also pushes the nested `clients/go/v<version>` tag the Go module proxy needs.
- **`sdk-python`** — version-linkage check + sdist/wheel build always; PyPI publish only on tag and only once `PYPI_API_TOKEN` is configured (plain twine — see the workflow comment for why not `pypa/gh-action-pypublish`).

### 3.13 `npm-publish.yml` — npm publish (fulla-oauth2)

**Trigger**: `workflow_dispatch` **only** (decision D7): the npm package publishes by manual dispatch, never on push, and never blocks a release. Steps: `npm ci` (Node 22) → version linkage (`--version-only`) → generated-tree drift gate (`--check --ts-only`) → `npm run check` (tsc + vitest) → `npm pack` (inspect the tarball that would ship) → `npm publish --access public` **only if the `NPM_TOKEN` secret exists**; otherwise a notice (build+pack-only verification — the expected pre-token state, which keeps the publish path exercised and reviewed).

### 3.14 What is deliberately NOT a GitHub Actions workflow

- **HTTP performance benchmarks** — `benchmarks/` (setup via `benchmarks/fulla/setup.sh`, scenarios via `benchmarks/fulla/run-scenario.sh`) is a **manual, out-of-band release gate** (five-scenario regression vs the prior release, same-machine back-to-back arms). CI shared runners give non-credible absolute numbers, so no benchmark workflow exists; a future relative-delta `benchmarks-regression.yml` is only a design item (`benchmarks/README.md` "Known limitations"). The `Performance_P1_Benchmark_*` cases run by `_build-test.yml` are in-process ctest entries, not throughput gates.

---

## 4. Frontend Gate (`_frontend.yml`)

The frontend gate runs for every CI run (it is part of the FAST gate, so it fails cheap and early). One Node job plus one Docker job:

| Job | What it does |
|---|---|
| `frontend` | UI-kit byte-sync gate → vitest unit/property tests (both apps) → ESLint → production builds (`tsc && vite build`) → **bundle-size budget gate** (`scripts/check-frontend-size.mjs`, #159) → Playwright e2e (both apps, API-mocked) |
| `frontend-image-smoke` (#160) | Path-filtered (`frontends/**`, `deploy/docker/**`, this workflow) `docker build` of **both frontend images with `push: false`** — the admin image (`frontends/admin/Dockerfile`) and the user frontend stage of `deploy/docker/Dockerfile` (`target: frontend-runtime`) — mirroring exactly what `release.yml` builds, so a context/lockfile/base-image regression fails the PR instead of the release. GHA layer cache keeps warm runs near the npm-ci cost. |

Budget baselines live in `scripts/check-frontend-size.mjs` (raw JS bytes: total + entry chunk per app, +10% headroom). Raise a baseline only with written justification in the PR — the entry-chunk cap doubles as the i18n AOT guard: the vue-i18n message compiler re-entering the bundle costs ~90 KB on the main chunk and trips the gate mechanically.

## 5. Core Job in Depth: `build-test`

`build-test` is a reusable workflow (`_build-test.yml`) invoked by `ci.yml` as a `{linux, windows, macos}` matrix. All three platforms run the same Conan + `cmake --preset` build and CTest suite; the database is enabled via matrix inputs only where needed.

### 5.1 Service Containers (linux matrix leg only)

In the Linux matrix leg, CI starts Postgres and Redis in Docker containers and confirms readiness with real queries (not just `pg_isready`):

| Service | Image | Port | Password |
|---|---|---|---|
| PostgreSQL | `postgres:17-alpine` | `5432` | `123456` |
| Redis | `redis:7-alpine` | `6379` | None (simplified CI configuration)|

> The PostgreSQL image matches the deploy default (`postgres:17-alpine`, since 2026-08-18) — CI therefore covers
> the major version actually deployed (including how migrations such as V025 partitioning / V026 behave on 17).

> **WARNING**: Redis runs without a password in CI, so the test configuration overrides it with the environment variable `FULLA_REDIS_PASSWORD=""`. The Windows/macOS matrix legs use `use_database=false` and fall back to the in-memory storage configuration (`config.ci.json`).

### 5.2 Build Cache Strategy

To speed up CI builds, Conan dependencies are cached:

| Cache | Cache key | Contents |
|---|---|---|
| **Conan dependency cache** | `conan-{OS}-v1-cpp17-{conanfile.py + conan.lock hash}` | The `~/.conan2` directory (third-party dependencies, including Drogon)|

A cold build takes roughly **15-20 minutes**; with a cache hit this drops to **3-5 minutes**.

### 5.3 Database Initialization

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

### 5.4 Test Execution

```bash
ctest -C Release --output-on-failure --timeout 1200
```

- `--output-on-failure` : print test stdout on failure
- `--timeout 1200` : the ceiling accounts for `OAuth2Tests` being the whole 61k-assertion binary (a 120 s ceiling used to report the suite as `(Timeout)`)

After the full ctest pass, the linux/windows legs re-run the headline **named test gates** individually (Error-Standardization Req 12.x properties, `Performance_P1_Benchmark_*`, E2E, RedirectUri integration) so a failure is attributable to the gated requirement.

### 5.5 Failure Log Upload

When tests fail, CI automatically packages and uploads the following as artifacts (retained for 7 days):

- `build/<preset>/Testing/` — CTest test reports
- `apps/server/logs/` — application runtime logs

---

## 6. Image Build and Signing

The CI pipeline itself does not build Docker images (except the frontend image *smoke* builds of §4). Multi-arch container image builds, GHCR pushes, cosign signing, and syft SBOMs are handled by `release.yml` when a SemVer tag (`vX.Y.Z`) is pushed — see §3.12. See also [Releases & Supply Chain Security](https://github.com/voidvec/fulla#releases--supply-chain-security).

---

## 7. Reproducing the CI Environment Locally

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

## 8. Multi-Platform Matrix

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
