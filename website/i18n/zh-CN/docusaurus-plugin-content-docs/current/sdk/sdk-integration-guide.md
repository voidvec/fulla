# SDK 集成指南（发布产物消费）

如何获取并集成 Fulla 的发布产物：SDK 二进制包（库 + 头 +
`fulla-*Config.cmake`）与 GHCR 容器镜像。运行时行为承诺（线程 / ABI /
异常 / 日志 / 插件注册）见 [SDK Runtime Contract](sdk-runtime-contract)，
本文只讲"怎么拿、怎么接"。发布流水线为
`.github/workflows/release.yml`（严格 SemVer tag `vX.Y.Z` 触发）。

> 非 C++ 消费者：官方维护的 **Python**（PyPI [`fulla-oauth2`](https://pypi.org/project/fulla-oauth2/)）与
> **Go**（`github.com/voidvec/fulla/clients/go`）HTTP 客户端开箱即用，
> 见 [clients/](https://github.com/voidvec/fulla/tree/master/clients)。

---

## 1. 发布产物清单

| 产物 | 位置 | 说明 |
|------|------|------|
| SDK 包 `fulla-sdk-<ver>-linux-x86_64.tar.gz` | GitHub Release 附件 | 8 个静态库 + `include/fulla/**` 头 + `lib/cmake/fulla-*/{Config,ConfigVersion,Targets}.cmake`（附 `.sha256`） |
| 后端镜像 | `ghcr.io/voidvec/fulla-backend:<ver>` | 多架构（amd64 + arm64），入口 `:5555`，`/health` 探活 |
| 用户前端镜像 | `ghcr.io/voidvec/fulla-frontend:<ver>` | nginx 静态托管，`:80` |
| 管理台镜像 | `ghcr.io/voidvec/fulla-admin:<ver>` | nginx 静态托管 `/admin`，`:80` |

镜像另有 `latest` 标签；`<ver>-amd64` / `<ver>-arm64` 为单架构中间标签。
服务器可执行文件**不在** SDK 包内——产品部署走镜像通道。

## 2. SDK 包前置条件（先读）

- **v1.x 只承诺源码级 SemVer，不承诺二进制 ABI**（契约 §2）。发布的
  `linux-x86_64` 静态库按 Release 流水线的工具链编译（ubuntu-24.04 /
  gcc / libstdc++ / C++17 / Conan 锁定依赖）；工具链不匹配时**请改用源码
  集成**（`add_subdirectory` 或自行 `cmake --install`，同一 SDK 面）。
- 第三方依赖（Drogon / OpenSSL / jsoncpp 等）**不在包内**。消费方用仓库根
  的 `conanfile.py` + `conan.lock` 解析同版本依赖，保证 `find_dependency`
  闭包与库编译时一致。

## 3. find_package 集成步骤

```bash
# 1) 解包
tar xzf fulla-sdk-1.3.1-linux-x86_64.tar.gz   # -> fulla-sdk-1.3.1-linux-x86_64/

# 2) 用仓库的 conanfile.py 解析依赖（生成 toolchain + 各依赖的 CMake config）
conan install <fulla-repo> --output-folder=deps --build=missing \
  -s build_type=Release -s compiler.cppstd=17

# 3) 配置消费工程：toolchain 供依赖解析，PREFIX_PATH 指向解包目录
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/deps/conan_toolchain.cmake \
  -DCMAKE_PREFIX_PATH=$PWD/fulla-sdk-1.3.1-linux-x86_64
cmake --build build -j
```

CMakeLists 侧：

```cmake
# 全栈宿主：一个包拉全闭包（common/oauth2/identity/storage-*/Drogon/OpenSSL/CURL）
find_package(fulla-drogon CONFIG REQUIRED)
target_link_libraries(my-host PRIVATE fulla::drogon)

# 或只取引擎面（无 Drogon 依赖）：
find_package(fulla-oauth2 CONFIG REQUIRED)
find_package(fulla-storage-memory CONFIG REQUIRED)
target_link_libraries(my-engine PRIVATE fulla::oauth2 fulla::storage::memory)
```

可用包与导出目标：`fulla-common`→`fulla::common`（另含
`fulla::common::testing`）、`fulla-oauth2`→`fulla::oauth2`、
`fulla-identity`→`fulla::identity`、
`fulla-storage-{memory,redis,postgres}`→`fulla::storage::{memory,redis,postgres}`、
`fulla-drogon`→`fulla::drogon`。版本兼容为 SameMajorVersion
（`find_package(fulla-drogon 1.0 CONFIG REQUIRED)` 可锁 major）。

参考消费方（随仓库 CI 持续验证）：

- `examples/full-stack-host/`：完整 HTTP 宿主，`find_package(fulla-drogon)`
  复用产品 controllers / OAuth2Plugin / views。Release 流水线用它对**安装
  前缀**做消费冒烟（`ctest -L SdkSmoke` 则对 build-tree 做同样验证）。
- `examples/third-party-host/`：最小引擎消费方，只链 Domain 层四个包。

## 4. 插件注册与 whole-archive（H1/F1/H5 口径）

- 插件本体当前以 **OBJECT 库**链入宿主，目标文件逐个直接链接，自注册符号
  不会被裁剪——**当前不需要 whole-archive**。
- 发布的 SDK 包中 `fulla::drogon` 是常规静态库，但插件注册走
  `config.json` `plugins[].name = "OAuth2Plugin"` 反射 + 显式
  `registerAllControllers()`（见 full-stack-host 的 main.cc），同样不依赖
  链接器保留未引用符号。若消费方自建**依赖静态初始化自注册**的封装，须
  自行 `-Wl,--whole-archive` 包裹对应库。
- 类名 / config schema 稳定性承诺见契约 §6。

## 5. 镜像使用

```bash
docker pull ghcr.io/voidvec/fulla-backend:1.3.1
```

三镜像与 `deploy/docker/docker-compose.yml` 的构建目标一一对应
（`backend-runtime` / `frontend-runtime` / `frontends/admin/Dockerfile`），
环境变量与挂载约定直接照搬 compose 文件的 `fulla-backend` 段
（`FULLA_DB_HOST` / `FULLA_REDIS_HOST` / `FULLA_AUTO_MIGRATE` 等）。

## 6. 发布流程（维护者）

1. 确认三源版本一致（`cmake/Version.cmake` 为单一事实源；api-diff 在 CI
   强制其与根 `CMakeLists.txt`、`conanfile.py` 一致）且 API 基线已按
   SemVer 规则更新（`tools/api-diff/`）。
2. （可选）本地刷新 CHANGELOG.md：
   `git cliff --unreleased --tag vX.Y.Z --prepend CHANGELOG.md`
   （配置见根目录 `cliff.toml`；发布工作流只生成 Release notes，
   不会从 tag ref 回推提交）。
3. 打严格 SemVer tag：`git tag v1.3.1 && git push origin v1.3.1`。带后缀
   的 tag（如 `v1.0.0-rc1`）**不会**触发发布。
4. `release.yml` 自动执行：tag/版本一致性校验 → SDK 打包 + 安装树消费
   冒烟 → amd64/arm64 原生构建三镜像 → 多架构 manifest（`<ver>` +
   `latest`）→ cosign keyless 按 digest 签名三镜像 + syft 生成 SPDX
   SBOM（三镜像 + 源码树）→ GitHub Release（git-cliff 生成 notes，
   挂 SDK 附件与全部 SBOM）。
5. `workflow_dispatch` 手动触发 = 干跑（全量构建但不推送、不发 Release）。

### 验证发布产物（消费方）

```sh
# 镜像签名（keyless：身份 = release.yml 工作流，无需公钥分发）
cosign verify ghcr.io/voidvec/fulla-backend:<ver> \
  --certificate-identity-regexp \
    'https://github.com/voidvec/[^/]+/.github/workflows/release.yml.*' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com

# SDK tarball 校验和（Release 附件）
sha256sum -c fulla-sdk-<ver>-linux-x86_64.tar.gz.sha256
```


## Quickstart：嵌入你自己的 Drogon 宿主

除 `find_package` 外只需两步（详见上文第 3 节的包引入）：

**1. 在宿主 `config.json` 激活插件**（自动注册协议路由与 Filter）：

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

**2. 用 `AuthorizationFilter` 保护业务 API**（全限定名 `fulla::drogon::filters::AuthorizationFilter`）：

```cpp
METHOD_LIST_BEGIN
ADD_METHOD_TO(UserApi::getProfile, "/api/me", drogon::Get,
              "fulla::drogon::filters::AuthorizationFilter");
METHOD_LIST_END
```

注意：链接 `fulla::drogon` 后 Controller/Filter 由 Drogon 启动时自动注册，勿手动调用初始化宏；PostgreSQL 存储需先执行 `apps/server/migrations/`（或 `FULLA_AUTO_MIGRATE=true`）。

> 本节合并自已退役的 plugin-integration.md（docs 治理 A2）。
