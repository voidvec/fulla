# SDK Runtime Contract

External contract statement: the threading model, ABI, exceptions, logging, and dependency boundaries that the fulla SDK (`fulla::common` / `fulla::oauth2` / `fulla::identity` / `fulla::storage-*` / `fulla::drogon`) commits to for consumers during v1.x.

This document is the single source of truth for external commitments; wherever SDK header comments conflict with it, this document prevails and the headers are to be fixed.

The unreleased S03 refresh hardening adds a repository migration requirement: see
[Refresh Tokens and Revocation Boundaries](../domains/token-refresh-and-revocation.md).
Old-only `ITokenRepository` implementations still compile, but refresh fails closed
until the new inspect/atomic-rotation/checked-revocation ports are implemented.

---

## 1. Threading Model

- Domain services (`libs/oauth2`, `libs/identity`) **do not own an event loop**; all asynchronous operations return via callbacks.
- **Callbacks may fire on any Drogon IO thread — the calling thread is not guaranteed**. Consumers must not assume thread affinity; when work must return to a specific thread, the consumer is responsible for dispatching it there.
- Read-only singletons (e.g. `JwkManager`) follow **init-once-then-read-only**: a one-shot `init()` completes before the service starts accepting requests; the object is then published as `shared_ptr<const T>` and never mutated afterwards. Concurrent reads by consumers are safe once SDK assembly is complete.
- Service objects hold `shared_ptr` repository handles to guarantee lifetimes; async continuations always capture `auto self = shared_from_this()` — `[this]` / `[&]` are forbidden.

## 2. ABI Stability

- v1.x **supports `find_package` source integration only and makes no binary-ABI commitment**.
- Semantic versioning covers the **source-level API** only: public headers under `include/fulla/**` follow SemVer, and breaking changes require a major-version bump (enforced in CI by the api-diff tool).
- Mixing precompiled binaries across compilers / STLs is out of scope; a dedicated ABI policy will be defined separately once the project moves to Conan binary-package distribution.
- Deprecation process: `[[deprecated]]` annotation + at least one minor-cycle transition period before removal.

## 3. Exception-Safety Contract

- Domain public APIs return **expected errors** via `Result<T, Error>`; exceptions are not used to express business failure.
- Exceptions are thrown only for unrecoverable programming errors (contract violations, assertion-level problems).
- Storage-level exceptions (e.g. `DrogonDbException`) **must be caught in the Adapter layer (`libs/storage-*`, `libs/drogon`) and converted to `Error` — they must not leak into Domain callbacks**. Consumers need no try/catch for storage exceptions inside Domain callbacks.

## 4. Logging Abstraction

- Domain code emits logs through the `common::ports::ILogger` port and **does not use Drogon `LOG_*` macros directly** (arch-guard enforces that the Domain layer does not include drogon headers).
- The SDK provides a default Drogon logging adapter implementation (the `libs/drogon` Adapter); consumers hosted outside Drogon can inject their own `ILogger` implementation as a replacement.

## 5. Dependency Declarations

- `with_social` / `with_webauthn` map to CMake's `WITH_SOCIAL` /
  `WITH_WEBAUTHN` switches and remove the corresponding services/controllers
  from the compiled surface.
- `with_identity=False` / `WITH_IDENTITY=OFF` is **unsupported**. The product
  and Postgres adapter still require `fulla::identity`; configuration fails
  explicitly. Keep identity enabled when configuring the repository.
- WebAuthn performs real W3C Level 2 CBOR/COSE and ES256 verification since
  v1.1.0 (#142); its typed read-only CborReader consumes libcbor. With WebAuthn
  disabled, Conan does not require libcbor, CMake does not find/link it, the
  service/crypto/CBOR tests are not compiled, and exported SDK configs do not
  require it. OpenSSL and jsoncpp remain shared dependencies for other features.

### 5.1 Supported combinations

Identity stays ON in every supported configuration. Both optional switches
default to ON; use matching Conan options rather than overriding only CMake.

| Social | WebAuthn | Compiled surface and dependency boundary |
|---|---|---|
| ON | ON | Full provider/WebAuthn services and controllers; libcbor required |
| OFF | ON | Provider login/link routes and social services removed; WebAuthn and libcbor remain |
| ON | OFF | WebAuthn routes, implementation and tests removed; no libcbor requirement |
| OFF | OFF | Core identity/MFA/TOTP/OAuth2 remain; neither optional service set nor libcbor is required |

Social OFF retains `GET /api/auth/providers` with an empty provider list and
the shared `POST /api/auth/providers/{provider}/authorize` route, which returns
404 when unavailable. Social OFF does not promise removal of shared HTTP or
storage adapters. Headers remain installed; existing `WITH_*` guards still
determine which declarations are available. An OFF library does not provide
the disabled service implementations. Link imported SDK targets and
use their exported feature definitions instead of defining `WITH_*` yourself.

Build switches do not delete stored WebAuthn credentials or social links.
Before deploying a trimmed variant, provide an available sign-in method for
accounts that rely on the removed feature. No automatic factor/data migration
is performed.

The checked-in Conan lock is a superset for supported configurations. Its
libcbor entry continues to lock ON builds; OFF graphs do not consume that entry.
Both build-tree and installed SDK consumers must resolve the selected closure.
No binary ABI compatibility is promised between feature configurations.

### 5.2 Reproduce the matrix checks

Prepare an isolated PostgreSQL/Redis test fixture as described in the
[testing guide](../contribute/testing-guide.md). The same `FULLA_DB_*` and
`FULLA_REDIS_*` overrides used by the normal test runner apply. Then run one
combination (Python 3.10+, Conan, CMake and the native toolchain required):

```bash
python3 tools/feature-matrix/run.py \
  --social ON --webauthn OFF \
  --build-dir build/social-on-webauthn-off \
  --evidence-dir docs-local/feature-matrix/social-on-webauthn-off
```

Use a new evidence directory for each attempt; earlier results are never
overwritten. Consumer/install directories must also be fresh (choose a fresh
build directory if an earlier attempt created them). Repeat for the other three rows. The runner resolves the locked
graph, builds with `FULLA_WERROR=ON`, checks configured sources/flags/exports,
runs the applicable CTest suite (including the complete service binary), and
builds/runs SDK consumers from both the build tree and a fresh install prefix.
OFF consumers explicitly forbid finding libcbor, and package/target locations
are checked against the intended prefix. Required tests must be registered and
pass without CTest skips, with live PG/Redis readiness verified. The unsupported
identity-OFF combination must fail in both Conan and CMake. Existing gtest/example warning
policies are unchanged; Werror applies to targets using the project warning helper.

Full-feature builds retain the legacy OAuth2/Admin endpoint scripts. Trimmed
builds instead run the feature-surface executable probe, which checks public
endpoints and asserts 404 for removed routes; enabled social builds also retain
the browser-bound state executable check. The complete default route golden
and OpenAPI fingerprint remain unchanged; only the expected set is reduced
for a trimmed build. Three trimmed Linux jobs run in Optional Feature Matrix
CI; the normal cross-platform CI covers ON/ON.

## 6. Plugin Registration and Configuration Contract (Host Integration)

- The `OAuth2Plugin` class name and the config `plugins[].name` reflective-loading contract **remain stable** (Option A): the class-name string in `"plugins":[{"name":"OAuth2Plugin","config":{...}}]` across the configs (`config.{json,dev,ci,prod,bench}.json`) and the schema of the `config{}` block are part of the product configuration contract and will not be renamed in v1.x.
- `fulla::drogon` is a **STATIC library**, including the plugin sources;
  the former separate OBJECT library has been absorbed into it.
- A host must retain the controller/plugin registration code it uses. Follow
  `examples/full-stack-host/`, which compiles the product bootstrap sources,
  explicitly registers controllers and wires plugin dependencies. A config
  class-name string alone does not force extraction of static-library objects.
- A wrapper relying only on unreferenced static self-registration must force
  the relevant objects into the executable (for example, whole-archive on
  applicable linkers) and verify registration in its own smoke test.
- See `examples/full-stack-host/` for HTTP host integration and `examples/third-party-host/` for engine-only integration.
