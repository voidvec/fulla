# Testing Strategy and Execution Guide (Testing Guide)

This document describes the project's test layering strategy, the coverage of each test file, and how to run the full test suite locally.

---

## 1. Test Prerequisites

Before running tests, make sure the following services are ready:

| Service | Address | Notes |
|---|---|---|
| **PostgreSQL** | `localhost:5432` | Database: `fulla_db` / user: `fulla_user` / password: `123456` |
| **Redis** | `localhost:6379` | Password: `123456` (consistent with `config.json`)|

> **Serialization warning**: the full test suite (`full_test.bat` /
> `full-test.sh`) and the benchmark stack (`benchmarks/fulla/setup.sh`)
> share port 5555 and the same PostgreSQL database. Never run them in
> parallel on one machine — start the benchmark setup only after the test
> suite (including its endpoint scripts) has fully finished, and vice
> versa.

> **Quick-start infrastructure**: if you use Docker, you can start the postgres and redis containers separately:
> ```powershell
> docker run -d -p 5432:5432 -e POSTGRES_USER=fulla_user -e POSTGRES_PASSWORD=123456 -e POSTGRES_DB=fulla_db postgres:17-alpine
> docker run -d -p 6379:6379 redis:7-alpine redis-server --requirepass 123456
> ```

---

## 2. Test Layering

Tests are compiled into **two categories of executables**:

1. **Per-library gtest binaries** (Domain layer, pure unit tests, no DB / no Drogon):
   - `libs/common/test/fulla-common-test` — `ConfigManager`, `ErrorCatalog`, `Result`, value objects
   - `libs/common/testing/test/fulla-common-testing-test` — deterministic verification of fake implementations (`FakeClock`/`FakeCryptoProvider`/`FakeLogger`, etc.)
   - `libs/oauth2/test/fulla-oauth2-test` — `TokenService`/`AuthorizationService`/`ClientService`/`JwkManager`/`Pkce`/`ScopeDecisionEngine`/`TokenCrypto`
   - `libs/identity/test/fulla-identity-test` — `AuthService`/`MfaService`/`TotpUtils`/`SessionManager`/`WebAuthnService`/social login (Google/WeChat/GitHub)
   - These use gtest (not `DROGON_TEST`) and are registered as independent ctest entries by each lib's `test/CMakeLists.txt` via `gtest_discover_tests`.

2. **Main test binary `tests/fulla-tests`** (`DROGON_TEST` framework, contains all layers that require Drogon/DB):

| Level | Directory | Coverage | External dependencies |
|---|---|---|---|
| **Level 1 — Unit tests** | `tests/unit/` (`config/`, `error/`, `utils/`, `validation/`, `plugin/`, `schema/`, `subject/`, `initorder/`) | Pure logic: error envelopes, password hashing, PKCE/CryptoUtils, RuleSet validation, config loading, OpenAPI generation | None |
| **Level 2 — Contract tests** | `tests/contract/` | Repository contract consistency across backends (Postgres/Redis/Memory): `IClientRepository`/`IGrantRepository`/`ITokenRepository`/`IConsentRepository`/`IUserRepository` | Memory always runs; Postgres/Redis are automatically skipped when `getPostgresClientOrNull()`/`getRedisClientOrNull()` return null |
| **Level 3 — Integration tests** | `tests/integration/` (`auth/`, `token/`, `storage/`, `concurrency/`, `error/`, `plugin/`) | Full business flows, concurrency races, error envelopes, plugin assembly | Postgres / Redis (in memory-only mode `-DFULLA_MEMORY_TESTS_ONLY=ON`, the Memory subset runs) |
| **Level 4 — Security tests** | `tests/security/` | SQL injection, XSS, command injection, CORS, token security, rate limiting | Postgres / Redis |
| **Level 5 — E2E/functional** | `tests/e2e-backend/`, `tests/performance/` | Complete OAuth2 flows, performance benchmarks | Postgres + Redis + Drogon App |

> Memory mode: configuring `-DFULLA_MEMORY_TESTS_ONLY=ON` lets the full suite run **without an external DB** (Postgres/Redis tests are automatically skipped) — this is how Windows CI does it.

> For security test case counts, functional test case counts, and coverage lists, see the header comments of the test files in each directory; this section no longer hardcodes specific counts (counts grow with each iteration — the measured statistics in §7 are authoritative).

### DROGON_TEST assertion style — bare boolean operators are forbidden inside `CHECK`/`REQUIRE` [#MUST]

drogon's `CHECK`/`REQUIRE` are **macros, not functions**: `CHECK_INTERNAL__` expands the expression to `(drogon::test::internal::Decomposer() <= expr)`, and macro argument substitution adds no parentheses, so a **bare `a || b` (or `a && b`) re-associates into `(Decomposer() <= a) || b`** — the result is silently wrong, with symptoms that look like "assertions randomly failing". Real case (PR #68 debugging): `CHECK(body.isMember("error") || body.isMember("code"))` failed, even though the raw body printed by `LOG_INFO` clearly contained the `error` key.

**Rule**: whenever `||`/`&&` appears in the top-level argument of `CHECK(...)`/`REQUIRE(...)`, one of the following must hold:

```cpp
// ✅ Split into two assertions (preferred — more precise failure messages)
CHECK(body.isMember("error"));
CHECK(body.isMember("code"));

// ✅ Wrap the whole expression in parentheses (outer parentheses bind the chained expression as a single operand to <=)
CHECK((a != std::string::npos || b != std::string::npos));

// ✅ Existing repo precedent: explicit (bool) cast (tests/e2e-backend/oauth2_flows/FunctionalTest.cc)
CHECK((bool)(response.find("code=") != std::string::npos ||
             response.find("error") != std::string::npos));

// ❌ Forbidden: bare top-level boolean chain (semantics broken after macro expansion)
CHECK(body.isMember("error") || body.isMember("code"));
```

Note: operators nested **inside call/subscript/sub-expression parentheses** (e.g. `CHECK(f(a || b))`, `CHECK(x == (a || b))`) are unaffected — they are evaluated before being bound to `<=`. The `CHECK_THROWS`/`REQUIRE_THROWS` family goes through the `EVAL__` path and is also unaffected.

**CI enforcement**: `tools/test/scripts/drogon_macro_bool_check.py` scans the `tests/` tree and fails on violations (a static-checks step, alongside the naming-convention check); `--selftest` can self-verify.

### Level 4 details — Security Tests

| Test file | Coverage |
|---|---|
| `SecurityTest.cc` | SQL injection, XSS, command injection, input validation, CORS, token security, rate limiting, health-check security |

Coverage highlights: input validation (injection/length/null values), authentication and authorization (invalid credentials, rate limiting), CORS in both directions, sensitive-data transmission, token security (invalid/missing authorization codes and refresh tokens), security headers (including HSTS), brute-force protection, health-check information leakage.

### Level 5 details — E2E / Functional Tests

| Test file | Coverage | Dependencies |
|---|---|---|
| `IntegrationE2ETest.cc` | Simulates the complete OAuth2 authorization code flow: HTTP request → authorize → login → token exchange → UserInfo verification | Postgres + Redis + a running Drogon App |
| `FunctionalTest.cc` | Complete OAuth2 flows, error handling, UTF-8/Emoji characters, health checks, RBAC, token lifecycle, input validation, rate limiting | Postgres + Redis |

Coverage highlights: the complete authorization code flow, error scenarios, UTF-8/Emoji boundaries, RBAC unauthorized paths, token lifecycle exception paths, overlong input, rate-limit detection, endpoint availability.

> Test case counts evolve with each version — **the `ctest -N` measurement is authoritative** (see §7 for the current full-suite baseline).

---

## 3. How to Run

### Option 1: via CTest (recommended)

```powershell
# Run after the build completes (directory is build/<preset>; on Windows Release it is windows-msvc)
cd build\windows-msvc
ctest -C Release --output-on-failure --timeout 120
```

> **Output policy**: by default only the logs of failed test cases and the final summary (`passed / failed / total`) are printed. To see the full output of every test case (including passing ones), add `--verbose` (`-V`); for complete silence with only the summary line, add `-Q`. `manage.sh test-backend -q` / `manage.ps1 test-backend -q` is equivalent to `-Q`.

### Option 2: run the test executable directly

The test executable automatically starts a Drogon App instance internally (synchronized via a semaphore in `test_main.cc`); **there is no need to start the backend service manually**.

```powershell
cd build\windows-msvc\tests\Release
.\fulla-tests.exe
```

### Option 3: use the manage scripts

`manage.ps1` (Windows) / `manage.sh` (Linux/macOS) wraps the same build + test pipeline used by CI:

```powershell
# Build and run the backend test suite (equivalent to manage.sh test-backend)
.\manage.ps1 test-backend

# Full loop: build + unit/integration tests + admin endpoint API tests
.\manage.ps1 full-test

# Run only the admin-endpoint / OAuth2-endpoint API scripts
.\manage.ps1 test-admin-endpoints
.\manage.ps1 test-oauth2-endpoints
```

### The full-test pipeline: three execution layers (and what is deduplicated)

`manage full-test` (backed by `scripts/backend/full_test.bat` /
`full-test.sh` / `full-test-docker.sh`) stacks three layers. Knowing how they
overlap explains what a full run actually executes:

1. **ctest layer** — `EndpointTests_OutOfProcess` (label `Endpoint`) is a
   regular ctest entry: it starts its own server, runs the 59 OAuth2 + 52
   admin endpoint scripts against it, then stops it
   (`tests/CMakeLists.txt`). It reports `SKIP_RETURN_CODE=77` when its
   environment (server binary / shell) is unusable.
2. **Dual-config layer** — `test.bat` / `test.sh` run the *entire* ctest
   suite **twice**: once with the standard `config.json` (PostgreSQL) and
   once with `config.ci.json` (memory storage). This duplication is
   intentional — it is the release-confidence signal that both storage
   backends pass the same suite.
3. **Manual endpoint layer** — the pipeline's own "start server → run the
   endpoint scripts → stop server" steps. Since layer 1 already runs the
   same scripts in the same standard configuration, this layer is skipped
   automatically when the ctest run's JUnit report
   (`build/<preset>/Testing/junit-config-standard.xml`, written by
   `test.bat`/`test.sh`) proves `EndpointTests_OutOfProcess` ran green in
   that invocation (#119). On any doubt — report missing, entry missing,
   skipped, or unreadable — the manual layer runs as before, so
   environments where the ctest entry bails out (77) keep their endpoint
   coverage path.

The same applies to the per-case `Contract.*` ctest entries: each is also
executed as part of the `OAuth2Tests` binary run. Their individual ctest
registrations exist only to provide a labeled entry point
(`ctest -L Contract`); they do not add a second execution of those cases
beyond the labeled entry.

---

## 4. Sample Test Output

```
All tests passed (N assertions in M tests)
```

If a failure occurs, the failed test name and assertion location are printed:
```
In test case SomeTestName
  SomeTestFile.cc:63  FAILED:
    CHECK(c.has_optional())
```

**Common failure causes**:
- Redis or PostgreSQL service not started → check that the services are reachable
- Redis password mismatch → check the `passwd` field in `config.json`
- Database not initialized → run the migration scripts under `apps/server/migrations/` (the backend also runs them automatically when `FULLA_AUTO_MIGRATE=true`)

---

## 5. Tests in CI

On every push to `master` or PR, GitHub Actions CI automatically:

1. Starts the Postgres and Redis service containers
2. Initializes the database schema
3. Builds the project
4. Runs `ctest`

See the [CI/CD Guide](../contribute/ci-cd-guide) for details.

---

## 6. Test Reports

Historical security/functional test reports, bug status analyses, and connection-leak verification reports are process archives that were moved out of the repository as part of documentation governance (kept locally by maintainers). Current test status is defined by CI and the scope of this section:

- **A fully green CI** is the merge gate (three-platform matrix; see the [CI/CD Guide](ci-cd-guide.md)).
- For security and functional coverage see the §2 Level 4/5 details; counts are authoritative per `ctest -N`.
- Actionable findings from the historical reports (e.g. security defects found in the April snapshot) have been fixed and preserved as regression test cases.

---

## 7. Test Coverage Summary

### Overall test status

> The numbers below are **measured statistics** (Windows MSVC Release build, no external DB, Postgres/Redis tests skipped). In an environment with Postgres+Redis (e.g. Linux CI or local WSL+Docker), the skipped contract/integration tests activate and the ctest entry count increases further.

| Test source | Passed | Failed | Total | Pass rate |
|---------|------|------|------|--------|
| **Per-library gtest binaries** (2026-06 baseline snapshot; the current full ctest count is 501 — the `ctest -N` measurement is authoritative) | 364 | 0 | 364 | 100% |
| **Main test binary ctest entries** (including the Contract label + the full OAuth2Tests run) | 450 | 0 | 450 | 100% |

> Note: the two rows overlap — the main binary contains all `DROGON_TEST` unit/integration tests (run as a single `OAuth2Tests` entry), while the per-library gtest binaries are pure Domain-layer unit tests compiled and run independently. Of the `450` ctest entries, 84 carry the `Contract` label (run them alone with `ctest -L Contract`).

### Code coverage

Measured line coverage (gcov, gcc 13.3 Debug build, WSL Ubuntu 24.04, Postgres+Redis active; ORM-generated `models/` excluded; measured at 7ba8068 with all 5 test binaries executed):

| Library | Line coverage | Notes |
|---|---|---|
| libs/common | 69.4% (318/458) | ErrorCatalog/ErrorTypes/ErrorContext/ConfigManager (driven by the per-lib gtest binary `fulla-common-test`); ConfigManager environment-related branches and some ErrorCatalog branches uncovered |
| libs/identity | **96.9%** (590/609) | Auth/Mfa/WebAuthn/Social/Totp/Session |
| libs/storage-memory | **97.1%** (431/444) | All methods of the Memory backend covered (the mandatory CI path) |
| libs/oauth2 | **92.1%** (627/681) | TokenService/AuthService/ClientService/JwkManager/Pkce |
| libs/storage-redis | 46.2% (306/663) | Contract tests cover the getClient/validate/grant/token/consent main paths; Lua scripts and transaction CRUD still to be covered |
| libs/storage-postgres | 43.9% (727/1657) | Contract tests cover the main paths; the remaining blind spots are transaction/error-fallback branches (require fault injection to trigger) |
| libs/drogon | **53.5%** (4807/8978) | admin 0%→55-69%, admin controllers 0%→91-100%; authorize/health/discovery/mfa/deviceauth/userselfservice/apidoc controllers reinforced; social OAuth controllers reinforced via mock injection (Google 38.3%, WeChat 30.6%, GitHub 32.5%); WebAuthn 39.2% (non-crypto stub, no authenticator needed) |
| **Overall** | **57.9%** (7806/13490) | Sum of the per-library rows above (the OVERALL of `scripts/measure_coverage.py` is exactly that per-library sum); +9.4pp improvement over the 48.5% baseline |

> The previous-round baseline was 48.5% (7091/14631); this round raised the overall figure to 57.9% through admin-layer HTTP integration tests + controller reinforcement + mock-injection tests for social OAuth/WebAuthn (`tests/common/SocialMockFixture.h` + the shared Fakes in `libs/identity/include/fulla/identity/testing/`). All of social OAuth's Google/WeChat/GitHub can run in memory mode via mock injection (the injection path writes no DB); the GitHub happy-path initially could not be covered in memory mode because `issueTokensForUser` called `getDbClient()` directly, and was subsequently refactored to persist through the `OAuth2Plugin::saveTokenPair` storage abstraction (see `Integration_P0_GitHubLogin_FakeExchange_ReturnsTokens` in `SocialLoginHttpTest.cc`), so the happy-path is now testable (GitHubController improved from 5.9% to 32.5%); WebAuthn is a non-crypto stub, fully testable in Postgres mode. Note: the 58.8% quoted by an earlier version of this document was a sum of stale per-library numbers (the 98.8% for common was outdated data — `libs/common/src` now has only 4 source files totaling 458 lines, measured at 69.4%); this table has been fully replaced with the values measured at 7ba8068. Remaining blind spots: storage-postgres transaction/error-fallback branches (require fault injection).

#### ⚠ Measured coverage requires running all 5 test binaries

The measured numbers depend on **all 5 test binaries** being executed (running only the main binary `fulla-tests` misses the domain-layer coverage contributed by the 4 per-lib gtest binaries, and common would be underestimated at ~60%):

1. `libs/common/test/fulla-common-test` (40 test cases)
2. `libs/common/testing/test/fulla-common-testing-test` (43 test cases)
3. `libs/identity/test/fulla-identity-test` (130 test cases)
4. `libs/oauth2/test/fulla-oauth2-test` (151 test cases)
5. `tests/fulla-tests` (450 ctest entries, including all `DROGON_TEST` unit/integration/contract/admin HTTP tests)

Run these 5 binaries in sequence under the coverage build directory, then aggregate the `.gcda` files.

#### ⚠ gcovr path-matching bug — use `scripts/measure_coverage.py` instead

`gcovr 8.6` falsely reports **0%** for some files (e.g. `ClientManagementService.cc`): raw `gcov` clearly shows `Lines executed:55.79% of 328`, yet gcovr's `--print-summary` lists only the file name without a percentage (gcovr's source-path matching handles `.gcov` output containing absolute paths + Drogon headers inconsistently). This is a known gcovr path-matching issue, not a zero-count bug (gcov flushing works correctly; see below).

**Reliable aggregation**: `scripts/measure_coverage.py` aggregates directly with `gcov -j` (JSON format, per file `{file, lines[{count, unexecuted_block}]}`), bypassing gcovr's text path matching. Usage:

```bash
cd <repo>
# First run all 5 binaries (see the previous section), then generate JSON + aggregate:
find build/linux-coverage/libs -path "*/src/*" -name "*.gcda" ! -path "*/models/*" \
  | xargs -I{} bash -c 'cd "$(dirname {})" && gcov -j "$(basename {})" >/dev/null 2>&1'
find build/linux-coverage/libs -path "*/src/*" -name "*.gcov.json.gz" ! -path "*/models/*" \
  | python3 scripts/measure_coverage.py
```

gcovr is still usable for per-file HTML reports (`--html-details`), but **for summary percentages `scripts/measure_coverage.py` is authoritative**.

#### Coverage toolchain

- `cmake/Coverage.cmake` (`oauth2_apply_gcov(target)`) adds `-fprofile-arcs -ftest-coverage` to every first-party library + test executable and explicitly links libgcov (GCC only; on Clang the profile runtime is provided automatically by the `-fprofile-arcs` link option, no libgcov).
- `tests/test_main.cc` explicitly calls `__gcov_dump()` before both `std::_Exit()` sites: because `_Exit` bypasses `atexit`, libgcov's counter flush does not run automatically (otherwise gcov reads all-zero counts). This is a known interaction between the Drogon test framework's fast exit and gcov, requiring a manual flush in the test main. (Note: the 4 per-lib gtest binaries exit normally without `_Exit`, so they need no manual flush.)
- Current phase target for coverage: 60% (57.9% measured at 7ba8068). The remaining blind spots concentrate in branches that are hard to test over HTTP: deep WebAuthn ceremony/crypto branches, UserSelfService (requires driving the auth pre-filter in reverse), storage-postgres transaction/error-fallback branches (require fault injection). Social OAuth controllers can now be covered in memory mode via the Fake injection of `SocialMockFixture.h` (the GitHub happy-path no longer depends on `getDbClient()` after the `saveTokenPair` storage-abstraction refactor). The ORM-generated `libs/storage-postgres/src/models/*.cc` files are excluded from the denominator.

---

## 8. Manual Validation & API Testing (Manual Validation)

Beyond the automated test suite, the project also provides tools and scripts for manually validating endpoint functionality.

### 8.1 PowerShell automation script
The project ships a complete OAuth2 endpoint test script: `scripts/backend/test-oauth2-endpoints.ps1`.

**Usage:**
```powershell
# Run the test, temporarily bypassing the execution policy
powershell -ExecutionPolicy Bypass -File scripts/backend/test-oauth2-endpoints.ps1
```
The script runs, in order: a health check, login, authorization code exchange, UserInfo access, and admin panel verification.

### 8.2 Multi-environment API testing (curl)
Different command-line tools vary in their support for curl syntax:

*   **PowerShell (recommended)**: use `Invoke-RestMethod`.
*   **Git Bash**: supports standard Unix single-quote syntax.
*   **CMD**: requires double quotes and escaping `&` as `^&`.

**Example: log in and get a JSON response**
```bash
# Git Bash example
curl -X POST http://127.0.0.1:5555/oauth2/login \
  -d 'username=admin&password=admin&client_id=fulla-portal&redirect_uri=http://localhost:5173/callback&json=true'
```

---

## 9. Troubleshooting

### 9.1 Common problems and remedies
*   **Server fails to start**: check whether port 5555 is occupied (`netstat -ano | findstr :5555`) and make sure the `config.json` path is correct.
*   **Login fails (400)**: confirm the username and password match and that the user exists in the database. Check that `redirect_uri` exactly matches the configuration.
*   **Token exchange fails**: the authorization code can be used only once and has a validity window. Make sure `client_id` and `client_secret` are correct.
*   **PowerShell script restriction**: if you see a "running scripts is disabled" message, use the `-ExecutionPolicy Bypass` parameter.

### 9.2 Debugging tips
*   **Log level**: when troubleshooting, temporarily set `log_level` in `config.json` to `DEBUG` (or even `TRACE`) for verbose output, and back to `INFO` once the issue is located. For the full six-level semantics and conventions see [observability.md §3.2](../operate/observability.md).
*   **Live logs**: use `Get-Content apps/server/logs/drogon.log -Wait -Tail 20` to monitor the running state.

---

**Related documentation**:
- [Security Architecture](../architecture/security-architecture.md) - security hardening and security architecture design
- [Data Consistency](../architecture/data-persistence.md) - data consistency and the threat model
- [API Reference](../domains/api-reference.md) - API interface documentation
