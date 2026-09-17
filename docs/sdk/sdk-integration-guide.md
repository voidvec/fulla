# SDK Integration Guide (Consuming Release Artifacts)

How to obtain and integrate fulla's release artifacts: the SDK binary package (libraries + headers + `fulla-*Config.cmake`) and the GHCR container images. For runtime behavior guarantees (threading / ABI / exceptions / logging / plugin registration), see the [SDK Runtime Contract](sdk-runtime-contract) — this document covers only how to obtain and wire everything up. The release pipeline is `.github/workflows/release.yml` (triggered by a strict SemVer tag `vX.Y.Z`).

> Non-C++ consumers: the officially maintained **Python** (PyPI [`fulla-oauth2`](https://pypi.org/project/fulla-oauth2/)) and **Go** (`github.com/voidvec/fulla/clients/go`) HTTP clients work out of the box; see [clients/](https://github.com/voidvec/fulla/tree/master/clients).

---

## 1. Release Artifact Inventory

| Artifact | Location | Notes |
|------|------|------|
| SDK package `fulla-sdk-<ver>-linux-x86_64.tar.gz` | GitHub Release attachment | 8 static libraries + `include/fulla/**` headers + `lib/cmake/fulla-*/{Config,ConfigVersion,Targets}.cmake` (with `.sha256`) |
| Backend image | `ghcr.io/voidvec/fulla-backend:<ver>` | Multi-arch (amd64 + arm64), entry port `:5555`, `/health` liveness probe |
| User frontend image | `ghcr.io/voidvec/fulla-frontend:<ver>` | nginx static hosting, `:80` |
| Admin console image | `ghcr.io/voidvec/fulla-admin:<ver>` | nginx static hosting of `/admin`, `:80` |

The images also carry a `latest` tag; `<ver>-amd64` / `<ver>-arm64` are single-arch intermediate tags. The server executable is **not** part of the SDK package — product deployment goes through the image channel.

## 2. SDK Package Prerequisites (Read This First)

- **v1.x guarantees only source-level SemVer, not binary ABI** (Contract §2). The published `linux-x86_64` static libraries are compiled with the Release pipeline's toolchain (ubuntu-24.04 / gcc / libstdc++ / C++17 / Conan-locked dependencies); if your toolchain does not match, **fall back to source integration** (`add_subdirectory`, or run `cmake --install` yourself — the same SDK surface).
- Third-party dependencies (Drogon / OpenSSL / jsoncpp, etc.) are **not** included in the package. Consumers resolve the same dependency versions using the repository root's `conanfile.py` + `conan.lock`, ensuring the `find_dependency` closure matches what the libraries were compiled against.

## 3. find_package Integration Steps

```bash
# 1) 解包
tar xzf fulla-sdk-1.3.2-linux-x86_64.tar.gz   # -> fulla-sdk-1.3.2-linux-x86_64/

# 2) 用仓库的 conanfile.py 解析依赖（生成 toolchain + 各依赖的 CMake config）
conan install <fulla-repo> --output-folder=deps --build=missing \
  -s build_type=Release -s compiler.cppstd=17

# 3) 配置消费工程：toolchain 供依赖解析，PREFIX_PATH 指向解包目录
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/deps/conan_toolchain.cmake \
  -DCMAKE_PREFIX_PATH=$PWD/fulla-sdk-1.3.2-linux-x86_64
cmake --build build -j
```

On the CMakeLists side:

```cmake
# 全栈宿主：一个包拉全闭包（common/oauth2/identity/storage-*/Drogon/OpenSSL/CURL）
find_package(fulla-drogon CONFIG REQUIRED)
target_link_libraries(my-host PRIVATE fulla::drogon)

# 或只取引擎面（无 Drogon 依赖）：
find_package(fulla-oauth2 CONFIG REQUIRED)
find_package(fulla-storage-memory CONFIG REQUIRED)
target_link_libraries(my-engine PRIVATE fulla::oauth2 fulla::storage::memory)
```

Available packages and exported targets: `fulla-common`→`fulla::common` (also provides `fulla::common::testing`), `fulla-oauth2`→`fulla::oauth2`, `fulla-identity`→`fulla::identity`, `fulla-storage-{memory,redis,postgres}`→`fulla::storage::{memory,redis,postgres}`, and `fulla-drogon`→`fulla::drogon`. Version compatibility is SameMajorVersion (`find_package(fulla-drogon 1.0 CONFIG REQUIRED)` pins the major version).

Reference consumers (continuously verified by the repository CI):

- `examples/full-stack-host/`: a complete HTTP host that uses `find_package(fulla-drogon)` to reuse the product controllers / OAuth2Plugin / views. The Release pipeline uses it to run a consumption smoke test against the **install prefix** (`ctest -L SdkSmoke` performs the same verification against the build tree).
- `examples/third-party-host/`: a minimal engine consumer that links only the four Domain-layer packages.

## 4. Plugin Registration and whole-archive (H1/F1/H5 Framing)

- The plugin itself is currently linked into the host as an **OBJECT library**: the object files are linked in directly, one by one, so self-registration symbols cannot be stripped — **whole-archive is not needed today**.
- In the published SDK package, `fulla::drogon` is a regular static library, but plugin registration goes through `config.json` `plugins[].name = "OAuth2Plugin"` reflection plus an explicit `registerAllControllers()` (see full-stack-host's main.cc); likewise, it does not rely on the linker retaining unreferenced symbols. If a consumer builds a wrapper that **depends on static-initialization self-registration**, they must wrap the corresponding library with `-Wl,--whole-archive` themselves.
- For the class-name / config-schema stability guarantees, see Contract §6.

## 5. Using the Images

```bash
docker pull ghcr.io/voidvec/fulla-backend:1.3.2
```

The three images correspond one-to-one to the build targets in `deploy/docker/docker-compose.yml` (`backend-runtime` / `frontend-runtime` / `frontends/admin/Dockerfile`); environment variables and mount conventions are taken directly from the compose file's `fulla-backend` service section (`FULLA_DB_HOST` / `FULLA_REDIS_HOST` / `FULLA_AUTO_MIGRATE`, etc.).

## 6. Release Process (Maintainers)

1. Confirm that the three version sources agree (`cmake/Version.cmake` is the single source of truth; api-diff enforces in CI that it matches the root `CMakeLists.txt` and `conanfile.py`) and that the API baseline has been updated according to SemVer rules (`tools/api-diff/`).
2. (Optional) Refresh CHANGELOG.md locally:
   `git cliff --unreleased --tag vX.Y.Z --prepend CHANGELOG.md`
   (configuration lives in the root `cliff.toml`; the release workflow only generates the Release notes and never back-fills commits from the tag ref).
3. Create a strict SemVer tag: `git tag v1.3.2 && git push origin v1.3.2`. Tags with a suffix (e.g. `v1.0.0-rc1`) do **not** trigger a release.
4. `release.yml` then runs automatically: tag/version consistency checks → SDK packaging + install-tree consumption smoke test → native builds of the three images for amd64/arm64 → multi-arch manifest (`<ver>` + `latest`) → cosign keyless signing of the three images by digest + syft-generated SPDX SBOMs (three images + source tree) → GitHub Release (notes generated by git-cliff, with the SDK attachment and all SBOMs attached).
5. A manual `workflow_dispatch` trigger = dry run (full build, but nothing is pushed and no Release is published).

### Verifying Release Artifacts (Consumers)

```sh
# 镜像签名（keyless：身份 = release.yml 工作流，无需公钥分发）
cosign verify ghcr.io/voidvec/fulla-backend:<ver> \
  --certificate-identity-regexp \
    'https://github.com/voidvec/[^/]+/.github/workflows/release.yml.*' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com

# SDK tarball 校验和（Release 附件）
sha256sum -c fulla-sdk-<ver>-linux-x86_64.tar.gz.sha256
```


## Quickstart: Embedding fulla into Your Own Drogon Host

Only two steps beyond `find_package` (for the package import, see Section 3 above):

**1. Activate the plugin in the host's `config.json`** (protocol routes and Filters are registered automatically):

```json
{
    "plugins": [
        {
            "name": "OAuth2Plugin",
            "dependencies": [],
            "config": {
                "storage_type": "postgres",
                "postgres": { "db_client_name": "default" },
                "redis": { "client_name": "default" }
            }
        }
    ]
}
```

**2. Protect business APIs with `AuthorizationFilter`** (fully qualified name `fulla::drogon::filters::AuthorizationFilter`):

```cpp
METHOD_LIST_BEGIN
ADD_METHOD_TO(UserApi::getProfile, "/api/me", drogon::Get,
              "fulla::drogon::filters::AuthorizationFilter");
METHOD_LIST_END
```

Note: once `fulla::drogon` is linked, Controllers/Filters are registered automatically at Drogon startup — do not invoke the initialization macros manually; the PostgreSQL storage requires running `apps/server/migrations/` first (or setting `FULLA_AUTO_MIGRATE=true`).

> This section was merged in from the retired plugin-integration.md (docs governance A2).
