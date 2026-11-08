# SDK Runtime Contract

External contract statement: the threading model, ABI, exceptions, logging, and dependency boundaries that the fulla SDK (`fulla::common` / `fulla::oauth2` / `fulla::identity` / `fulla::storage-*` / `fulla::drogon`) commits to for consumers during v1.x.

This document is the single source of truth for external commitments; wherever SDK header comments conflict with it, this document prevails and the headers are to be fixed.

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
  v1.1.0 (#142); its typed read-only CborReader consumes libcbor. `libcbor` remains
  an **unconditional dependency**, including when WebAuthn is disabled,
  because `WebAuthnCrypto.cc` and its tests are still compiled. Disabling a
  feature does not currently guarantee removal of all of its dependencies.

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
