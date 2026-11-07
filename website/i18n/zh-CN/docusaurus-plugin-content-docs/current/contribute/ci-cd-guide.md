# CI/CD 流水线指南 (CI/CD Guide)

本文档说明项目的持续集成与持续交付（CI/CD）机制，基于 **GitHub Actions**。

---

## 1. 流水线概览

CI 配置位于 `.github/workflows/ci.yml`，由三个 fail-fast 串联的 Job（含一条可复用工作流矩阵）组成：

```
Push/PR 到 master (及 workflow_dispatch)
        │
        ├── FAST gate
        │     ├── static-checks (ubuntu-24.04) — 源码级守卫：
        │     │     arch-guard / double-move 捕获守卫 (#183) /
        │     │     migration-check / api-diff /
        │     │     测试命名 / manage 脚本对等 / OpenAPI 校验 /
        │     │     OpenAPI 治理门（三层一致性 + 版本同步）
        │     └── frontend (_frontend.yml) — vitest 单元/属性测试、ESLint、
        │           生产构建 + 包体积预算门 (#159)、Playwright e2e、
        │           前端 Docker 镜像构建冒烟 (#160，路径过滤 frontends/** + deploy/docker/**)
        │
        ├── MAIN gate
        │     └── build-test (_build-test.yml × {linux, windows, macos} 矩阵)
        │           ├── 安装系统依赖 / Conan
        │           ├── 配置并构建 (Conan + cmake --preset，带缓存)
        │           ├── [linux] 启动 Postgres/Redis 容器并等待就绪
        │           ├── [linux] 初始化数据库 Schema
        │           ├── 运行 ctest + 命名发布门禁
        │           └── [失败时] 上传测试日志 Artifact
        │
        └── RELEASE gate
              └── sdk-smoke (_sdk-smoke.yml) — 全栈 find_package 冒烟
```

> `ci.yml` 只是 **per-PR 入口**。`.github/workflows/` 下共有 **13 个工作流文件**；其余的各有自己的触发（路径过滤的 PR 门、定时、Tag 或手动 dispatch）。第 3 节全量覆盖。

---

## 2. 触发条件

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

- **Push to master**：每次合并到 master 后自动触发全量检查。
- **Pull Request**：每次 PR 创建/更新时在合并前自动触发，作为门禁检查。
- **workflow_dispatch**：支持手动触发。
- **并发控制**：同一分支的新运行会取消其进行中的旧运行。

---

## 3. 工作流全量清单（`.github/workflows/` 下全部 13 个文件）

13 个文件中有 3 个是**可复用工作流**（`_build-test.yml`、`_frontend.yml`、`_sdk-smoke.yml`，均为 `on: workflow_call`）——只在 `ci.yml` 调用时运行；其余 10 个是独立顶层工作流，各有自己的触发条件与路径过滤。

下文使用的分类口径：

- **门禁（Gate）**——无 `continue-on-error`；失败即红，`pull_request` 触发的工作流失败会阻塞合并。
- **参考（Reference）**——步骤带 `continue-on-error: true`：只报告（step summary、注解、Artifact），本身不会让运行失败。
- **合并后 / 仅发布 / 手动**——不在 PR 上跑；只在 `master`/`release/**` push、Tag 或 `workflow_dispatch` 时运行。

### 3.0 汇总表

| 文件 | 触发（+ 路径过滤） | 干什么 | 分类 |
|---|---|---|---|
| `ci.yml` | push master / PR master / dispatch（无路径） | FAST → MAIN → RELEASE 三道门：静态检查、前端、三平台构建测试、SDK 冒烟 | 门禁 |
| `_build-test.yml` | `workflow_call`（被 `ci.yml` 调用） | Conan + `cmake --preset` 构建、全量 ctest、命名门禁、日志 Artifact | 门禁 |
| `_frontend.yml` | `workflow_call`（被 `ci.yml` 调用） | UI-kit 同步、vitest、ESLint、tsc 构建、包体积预算、Playwright e2e、前端镜像冒烟 | 门禁 |
| `_sdk-smoke.yml` | `workflow_call`（被 `ci.yml` 调用） | 全栈构建 + `SdkSmoke.FullStack` 外部 `find_package` 消费者 | 门禁 |
| `openapi-governance.yml` | PR master / dispatch（路径：`apps/server/openapi.yaml`、`tools/openapi-governance/**`、自身） | oasdiff 破坏性变更门，base vs PR | 门禁 |
| `clients-sdk.yml` | PR master / push master / dispatch（路径：`clients/**`、`tools/clients/**`、`apps/server/openapi.yaml`、自身） | Python/Go/TS 三客户端：生成树漂移门 + 版本联动 + 单元测试 | 门禁 |
| `security.yml` | push master / PR master / 每周 cron（周一 03:00 UTC）/ dispatch（无路径） | `conan.lock` 依赖 EOL 扫描 + 密钥/敏感文件卫生检查 | 门禁（cron 腿：对新增 EOL 依赖报警） |
| `sanitizer.yml` | PR master / push master / 每夜 / dispatch（路径：`libs/**`、`apps/server/**`、`tests/**`、`cmake/**`、`CMakePresets.json`，PR 时含自身） | `asan-ubsan`：真实 Postgres+Redis 下的全量 ctest；`tsan-nightly`：TSan 全量 ctest | `asan-ubsan`：门禁；`tsan-nightly`：每夜 cron + dispatch（2026-11-07 补上 cron）|
| `coverage.yml` | PR master / push master / 每夜 / dispatch（路径：`libs/**`、`apps/server/**`、`tests/**`、`tools/coverage/**`，PR 时另含脚本/cmake/自身） | gcov 插桩构建、逐库覆盖率报告、对基线的 ratchet 比对 | PR 腿：参考（60 分钟）；**gate 腿：套件完成 + ratchet 硬门禁**（push master + 每夜，150 分钟） |
| `website.yml` | push master / PR（任意分支，路径：`docs/**`、`blog/**`、`website/**`、`scripts/gen-llms-txt.mjs`、自身）/ dispatch | llms.txt 新鲜度门 + Docusaurus 构建（`onBrokenLinks: 'throw'`）；master/dispatch 时部署 Pages | 门禁（build）；deploy：仅 master push / dispatch |
| `interop.yml` | push master / push `release/**` / dispatch（无路径） | RP 互操作冒烟：三个 RP 库 + nextjs-demo 浏览器驱动对真实 fulla 栈 | 合并后 / release 分支门禁（刻意不进 PR 热路径） |
| `release.yml` | push Tag `vX.Y.Z`（严格 SemVer）/ dispatch（dry run） | 版本校验、SDK tar 包、多架构镜像、cosign + SBOM、GitHub Release、PyPI | 发布门禁（仅 Tag） |
| `npm-publish.yml` | 仅 dispatch | `fulla-oauth2` npm 包：校验、pack、发布（需 `NPM_TOKEN`） | 手动；不阻塞任何发布 |

### 3.1 `ci.yml` — CI

**触发**：push master / PR master / workflow_dispatch；并发按 ref 取消被顶替的旧运行。

单一入口，经 `needs` 串联三道门（见 §1 树）：

- **`static-checks`**（ubuntu-24.04，10 分钟）——纯源码、不编译：`arch_guard.py`（domain 库保持框架无关）、`double_move_guard.py`（#183 崩溃守卫）、`migration_check.py`、`api_diff.py`（对导出 SDK 头与 `tools/api-diff/api-baseline.txt` 的 SemVer 守卫）、测试命名校验、`drogon_macro_bool_check.py`、`manage-parity-check.sh`、OpenAPI 规范校验 + 规范治理门（路由 == 文档 == `openapi.yaml`，版本同步）。
- **`frontend`**——调用 `_frontend.yml`（见 §3.3 与 §4）。
- **`build-test`**——以 `{linux, windows, macos}` 矩阵调用 `_build-test.yml`（`fail-fast: false`；每腿 60 分钟，全部 `-DFULLA_WERROR=ON`；见 §3.2 与 §5）。
- **`sdk-smoke`**——build-test 之后调用 `_sdk-smoke.yml`（见 §3.4）。

### 3.2 `_build-test.yml` — 可复用 Build & Test（MAIN 门）

一套参数化 Job 体三平台共用（差异只经输入表达）。步骤：系统依赖 / Conan 安装 → Conan 缓存 → 配置 + 构建 → （linux）Postgres/Redis 容器、就绪探测、schema + seed 初始化 → 全量 ctest → 命名门禁（`run_named_test_gates`，仅 linux+windows）：错误标准化 Req 12.x 测试、`Performance_P1_Benchmark_*` 进程内用例、E2E 与 RedirectUri 集成条目 → 失败时上传测试日志（保留 7 天）。详细行为见 §5。

### 3.3 `_frontend.yml` — 可复用前端质量门（FAST 门）

两个 Job：`frontend`（Node 20）与 `frontend-image-smoke`（#160，Docker，经 `dorny/paths-filter` 路径过滤到 `frontends/**` + `deploy/docker/**`）。详细行为见 §4。

### 3.4 `_sdk-smoke.yml` — 可复用 SDK 冒烟（RELEASE 门）

仅 Linux（单一权威 SDK 可消费性证明），60 分钟。以 `BUILD_EXAMPLES=ON` 构建全栈（生成 build-tree 包配置）并跑 `ctest -L SdkSmoke`：`SdkSmoke.FullStack` 用 `ctest --build-and-test` 把 `examples/full-stack-host` 作为一个真实外部 `find_package` 消费者来配置、构建、运行。冒烟 host 用内存存储（`config/smoke.json`）——无需数据库。门禁：是。

### 3.5 `openapi-governance.yml` — OpenAPI 治理

**触发**：PR master（路径：`apps/server/openapi.yaml`、`tools/openapi-governance/**`、工作流自身）+ workflow_dispatch；5 分钟；需 `fetch-depth: 0` 从 `origin/master` 提取 base 规范。

运行**固定版本 oasdiff v1.29.1** 的 `breaking` 比对 base 分支规范与 PR 规范；任何破坏性变更（路径删除、请求体收紧、响应收窄等）都会失败，**除非**随大版本号升级，或在 `tools/openapi-governance/oasdiff-breaking-ignore.md` 中带理由显式豁免。门禁：是。本地可用同一 `oasdiff` 命令复现。

### 3.6 `clients-sdk.yml` — Client SDKs

**触发**：PR master / push master / dispatch，路径过滤到 `clients/**`、`tools/clients/**`、`apps/server/openapi.yaml` 与工作流自身——无关 PR 不受影响。刻意不放进 `ci.yml` 的 static-checks（需要 Python + Go + Node 工具链和固定版本生成器）。

三个 15 分钟 Job：

- **`python`**——固定 `openapi-python-client==0.29.0`；**漂移门**（`regen_clients.py --check --python-only`：已提交的生成树必须与 `openapi.yaml` 一致）+ pytest 单元测试（MockTransport；集成套件自动跳过）。
- **`go`**——build + vet + gofmt；**漂移门**（`--go-only`）+ `go test ./...`（httptest；集成测试在 `-tags integration` 之后）。
- **`ts`**——Node 22；**漂移门**（`--ts-only`）+ **版本联动**（`--version-only`：`package.json` == `pyproject.toml` == `cmake/Version.cmake`）+ `npm run check`（tsc 产物 + vitest 红线）。

门禁：是，三个 Job 都是。本地等价命令见工作流头部注释。

### 3.7 `security.yml` — Security

**触发**：push master / PR master / **每周一 03:00 UTC cron** / dispatch——定时存在的意义是即使没有提交落地，也能发现新 EOL 的依赖。两个 5 分钟 Job，push/PR 时都是门禁：

- **`dependency-eol`**——`tools/security/dependency_eol_check.py` 扫 `conan.lock`（策略下限：openssl >= 3.0、zlib >= 1.2.13、libcurl >= 8.4.0）。
- **`secret-hygiene`**——`scripts/security-check.sh`：被跟踪的 `.env` 文件、硬编码密钥模式、`.gitignore` 覆盖、配置文件权限。

### 3.8 `sanitizer.yml` — Sanitizers

**触发**：PR master / push master / dispatch，路径过滤到 `libs/**`、`apps/server/**`、`tests/**`、`cmake/**`、`CMakePresets.json`（工作流自身仅 PR 时在列）。

- **`asan-ubsan`**（门禁，60 分钟）——GCC Debug 构建，`linux-asan-ubsan` preset（UBSan 经 `halt_on_error=1` 达到遇错即止语义，ASan 开泄漏检测），对真实 Postgres 17 + Redis 7 容器跑全量 ctest（单测 900 秒上限），集成套件同样覆盖。失败上传日志。分诊策略：真实缺陷同 PR 修复；第三方/系统调用误报在 `tools/sanitizers/` 下配带说明的抑制文件。
- **`tsan-nightly`**（TSan，90 分钟）——`if: schedule || dispatch`；TSan 与 ASan/UBSan 互斥，故独立 Job 用 `linux-release-tsan` preset 跑**全量**套件。经 `schedule: 0 3 * * *` 每夜运行（cron 于 2026-11-07 补上——此前虽有 nightly 之名，实际只能手动 dispatch 触发）。

### 3.9 `coverage.yml` — Coverage

**触发**：PR master / push master / 每夜定时 / dispatch，路径过滤到 `libs/**`、`apps/server/**`、`tests/**`、`tools/coverage/**`（PR 腿另盯 `scripts/measure_coverage.py`、`cmake/Coverage.cmake`、`CMakePresets.json` 与工作流自身）。同一复用体（`_coverage-run.yml`）以**双腿**运行：

- **advisory 腿**（PR，60 分钟）：`FULLA_TEST_COVERAGE` 插桩构建、尽力而为的 ctest（`continue-on-error`）、显式 `gcov -j -p` JSON 生成（`.gcda`/`.gcov.json.gz` 为零即硬失败——假绿报告本身就是失败）、逐库汇总、ratchet 仅作参考。此处回归是"需要排查的信号"，不阻塞。
- **gate 腿**（push master + 每夜 03:30 UTC + dispatch，150 分钟）：同一执行体，但**套件必须跑完**（ctest 失败向上传播）且 **ratchet 为硬门禁**——不增回归在合并后一天内被挡下。

两腿共享上述 2–5 步（gcov JSON 空数据硬失败；`measure_coverage.py --ratchet tools/coverage/ratchet-baseline.json` 逐库与已提交基线比对——见[测试指南 §7](testing-guide.md)；原始 gcov JSON + 测试日志上传为 Artifact）。

**为什么双腿（2026-11-07 实测）**：6.1 万断言的全量套件在 CI linux runner 的 Release 速度下约 3.5 分钟；gcov 插桩使进程内服务端慢 10–20×，插桩后套件需 35–70 分钟纯测试时间——塞不进还要容纳约 29 分钟构建的 60 分钟 PR 腿。套件分片经评估被否决：`OAuth2Tests` 是单体测试二进制上的**单个** `add_test()`，按用例 `ctest -r` 会为每个用例重新拉起服务端（只有纯仓储的 Contract 套件付得起这个代价），拆分二进制则是对 3.5 分钟套件不成比例的构建系统改造。专用大 runner 可行但无必要——时限如实放大后（约 100 分钟）标准托管 runner 就装得下（上限 360）。

### 3.10 `website.yml` — Website

**触发**：push master / **任意分支**的 PR（路径：`docs/**`、`blog/**`、`website/**`、`scripts/gen-llms-txt.mjs`、工作流）/ dispatch（合并前从分支手动上线）。并发组 `pages` **不**取消进行中的部署。

- **`build`**（门禁）——
  1. **llms.txt 新鲜度门**：用 `scripts/gen-llms-txt.mjs` 从 `docs/` 重新生成 `website/static/llms.txt` + `llms-full.txt` 并 `git diff --exit-code`，已提交的 AI-native 产物不可能过期漂移；
  2. Docusaurus 构建（`npm ci && npm run build`），`website/docusaurus.config.js` 中 **`onBrokenLinks: 'throw'`**——死链直接构建失败，取代人工链接检查；
  3. 上传 Pages Artifact。
- **`deploy`**——仅 master push 或 dispatch；发布到 GitHub Pages（需 Pages Source = GitHub Actions）。

### 3.11 `interop.yml` — RP 互操作冒烟

按工作流自身头注释：**v1.5.0 P0 出口门**（"三个库，零告警"）。四个驱动——三个标准 RP 库（`openid-client` v6 / `authlib` / `go-oidc` + `x/oauth2`）以**严格模式**对真实 fulla 栈驱动 授权码 + PKCE → token → userinfo → refresh → end-session 全链路，外加 P1-5 的 **nextjs-demo Playwright 冒烟**第四驱动：通过第一方 `fulla-oauth2` TS SDK 驱动**真实浏览器**（落地页 → fulla 登录表单 → 校验 state+PKCE 的回调 → profile/userinfo → 受保护 API → RP 发起的登出）——与三个 json 模式驱动是不同的配方。

**触发**：push 到 `master` 或 `release/**` + workflow_dispatch。刻意**不**在 per-PR 热路径上（运行成本）；它是合并后/release 分支的互操作门。单一 Job（45 分钟）：只构建 `fulla-server` 目标（linux-release），起 Postgres 17 + Redis 7，执行 migrations + seeds，启动服务并轮询 `/health`，然后按序跑四个驱动（Node、Python、Go 在 `tests/interop/go` 内运行，然后 demo：构建 `clients/ts`、`npm ci`、Playwright chromium、`next start -p 8080`、`npx playwright test`）。失败时打印服务端日志尾部。详见 `tests/interop/README.md`。

### 3.12 `release.yml` — Release

**触发**：push **严格 SemVer Tag**（`v1.2.3` 模式；`v1.0.0-xxx` 之类带后缀的 Tag 刻意**不**发布）+ workflow_dispatch 作为 **dry run**（全部构建但不推送；manifest/release 跳过）。并发配置永不取消发布到一半的运行。Job：

- **`version-check`**——Tag 必须等于 `cmake/Version.cmake`；重跑 OpenAPI 规范治理检查与 api-diff 冻结面检查（后者还交叉校验 cmake vs CMakeLists vs conanfile）。
- **`sdk-package`**——Linux x86_64 SDK tar 包（8 个静态库 + 头文件 + CMake 包配置，经 `cmake --install`），并通过在**已安装**前缀下配置 + 构建 `examples/full-stack-host` 验证可消费性；附 sha256。
- **`images`**——backend / frontend / admin 镜像，amd64 + arm64，各自在**原生** runner 上构建（不用 QEMU——C++ 构建在模拟下不现实），以 `<version>-<arch>` 推 GHCR（仅 Tag 运行时推）。
- **`manifest`**（仅 Tag）——imagetools 合并多架构 manifest `<version>` + `latest`，随后对 manifest digest 做 cosign **keyless** 签名并生成逐镜像 SPDX SBOM（syft）。
- **`github-release`**（仅 Tag）——git-cliff（约定式提交）生成 Release notes，带 CHANGELOG 回退 + 拼接破坏性变更条目 + 验证说明；附 SDK tar 包与 SBOM；同时推送 Go module proxy 需要的嵌套 `clients/go/v<version>` Tag。
- **`sdk-python`**——版本联动检查 + sdist/wheel 构建（总是执行）；仅 Tag 且配置了 `PYPI_API_TOKEN` 时发布 PyPI（普通 twine——为何不用 `pypa/gh-action-pypublish` 见工作流注释）。

### 3.13 `npm-publish.yml` — npm publish (fulla-oauth2)

**触发**：**仅** `workflow_dispatch`（决策 D7）：npm 包靠手动 dispatch 发布，从不在 push 时跑，也从不阻塞发布。步骤：`npm ci`（Node 22）→ 版本联动（`--version-only`）→ 生成树漂移门（`--check --ts-only`）→ `npm run check`（tsc + vitest）→ `npm pack`（检查将要发布的 tar 包）→ **仅在 `NPM_TOKEN` secret 存在时** `npm publish --access public`；否则给一条 notice（只做构建+pack 验证——这是发 token 前的预期状态，目的是让发布路径始终保持被演练和评审）。

### 3.14 刻意不用 GitHub Actions 工作流的门

- **HTTP 性能基准**——`benchmarks/`（`benchmarks/fulla/setup.sh` 起栈、`benchmarks/fulla/run-scenario.sh` 跑场景）是**手动的、带外发布的门禁**（五场景对上一版本回归，同机背靠背臂）。CI 共享 runner 的绝对数字不可信，因此没有基准工作流；未来的相对差值 `benchmarks-regression.yml` 只是设计项（见 `benchmarks/README.md` "Known limitations"）。`_build-test.yml` 跑的 `Performance_P1_Benchmark_*` 是进程内 ctest 条目，不是吞吐门禁。

---

## 4. 前端门禁（`_frontend.yml`）

前端门禁随每次 CI 运行（属于 FAST 门，便宜且早失败）。一个 Node Job 加一个 Docker Job：

| Job | 做什么 |
|---|---|
| `frontend` | UI-kit 字节同步门 → vitest 单元/属性测试（双端）→ ESLint → 生产构建（`tsc && vite build`）→ **包体积预算门**（`scripts/check-frontend-size.mjs`，#159）→ Playwright e2e（双端，API mock） |
| `frontend-image-smoke` (#160) | 路径过滤（`frontends/**`、`deploy/docker/**`、本工作流）后以 `push: false` `docker build` **两个前端镜像**——admin 镜像（`frontends/admin/Dockerfile`）与 `deploy/docker/Dockerfile` 的 user 前端阶段（`target: frontend-runtime`）——与 `release.yml` 构建的完全一致，让 context/lockfile/基础镜像回归在 PR 上而非发布时暴露。GHA 层缓存让热运行成本接近 npm-ci。 |

预算基线在 `scripts/check-frontend-size.mjs`（原始 JS 字节：每端总量 + entry chunk，+10% 余量）。上调基线必须在 PR 中给出书面理由——entry chunk 上限兼任 i18n AOT 守卫：vue-i18n 消息编译器回到主包要花约 90 KB，会机械地触发门禁。

## 5. 核心 Job 详解：`build-test`

`build-test` 是一条可复用工作流（`_build-test.yml`），由 `ci.yml` 以 `{linux, windows, macos}` 矩阵调用，三个平台执行同一套 Conan + `cmake --preset` 构建与 CTest 测试，仅在需要时通过矩阵输入启用数据库。

### 5.1 Service Containers（仅 linux 矩阵腿）

CI 在 Linux 矩阵腿中用 Docker 容器启动 Postgres 和 Redis，并通过真实查询（非仅 `pg_isready`）确保就绪：

| Service | 镜像 | 端口 | 密码 |
|---|---|---|---|
| PostgreSQL | `postgres:17-alpine` | `5432` | `123456` |
| Redis | `redis:7-alpine` | `6379` | 无（CI 环境简化配置）|

> PostgreSQL 镜像与 deploy 默认（`postgres:17-alpine`，2026-08-18 起）对齐——CI 覆盖的
> 就是部署目标大版本（含 V025 分区/V026 等 migration 在 17 上的行为）。

> **WARNING** **注意**：CI 中 Redis 无密码，因此测试配置通过环境变量 `FULLA_REDIS_PASSWORD=""` 覆盖。Windows/macOS 矩阵腿 `use_database=false`，改用内存存储配置（`config.ci.json`）。

### 5.2 构建缓存策略

为加速 CI 构建速度，对 Conan 依赖做缓存：

| 缓存 | 缓存键 | 内容 |
|---|---|---|
| **Conan 依赖缓存** | `conan-{OS}-v1-cpp17-{conanfile.py + conan.lock hash}` | `~/.conan2` 目录（第三方依赖，含 Drogon）|

初次构建约需 **15-20 分钟**；缓存命中后降至 **3-5 分钟**。

### 5.3 数据库初始化

测试前执行 migration 脚本初始化数据库：

```bash
# 按顺序执行所有 migration 文件
for f in apps/server/migrations/V*.sql; do
    psql -h localhost -U fulla_user -d fulla_db -f "$f"
done

# 执行 seed 数据（开发/测试环境）
for f in apps/server/seed/*.sql; do
    psql -h localhost -U fulla_user -d fulla_db -f "$f"
done
```

> **注意**: 旧的 `sql/001_*.sql` ~ `sql/004_*.sql` 文件已废弃并删除，所有 schema 定义统一在 `apps/server/migrations/` 目录中管理。

### 5.4 测试执行

```bash
ctest -C Release --output-on-failure --timeout 1200
```

- `--output-on-failure` : 失败时打印测试标准输出
- `--timeout 1200` : 上限考虑了 `OAuth2Tests` 是整个 6.1 万断言二进制（旧的 120 秒上限曾把整套报告成 `(Timeout)`）

全量 ctest 之后，linux/windows 腿会再逐条运行 headline **命名门禁**（错误标准化 Req 12.x 属性、`Performance_P1_Benchmark_*`、E2E、RedirectUri 集成），让失败可直接归因到对应需求。

### 5.5 失败日志上传

测试失败时，CI 会自动打包并上传以下内容作为 Artifact（保留 7 天）：
- `build/<preset>/Testing/` — CTest 测试报告
- `apps/server/logs/` — 应用运行日志

---

## 6. 镜像构建与签名

CI 流水线本身不构建 Docker 镜像（§4 的前端镜像*冒烟*构建除外）。容器镜像的多架构构建、推送 GHCR、cosign 签名与 syft SBOM 由 `release.yml` 在打 SemVer Tag（`vX.Y.Z`）时完成——见 §3.12。另见 [Releases & Supply Chain Security](https://github.com/voidvec/fulla#releases--supply-chain-security)。

---

## 7. 本地复现 CI 环境

如需本地模拟 CI 行为：

```powershell
# 1. 启动基础设施（CI 中使用 Service Container，本地用 Docker）
docker run -d -p 5432:5432 -e POSTGRES_USER=fulla_user -e POSTGRES_PASSWORD=123456 -e POSTGRES_DB=fulla_db postgres:17-alpine
docker run -d -p 6379:6379 redis:7-alpine

# 2. 初始化数据库
$env:PGPASSWORD = "123456"
Get-ChildItem "apps\server\migrations\V*.sql" | Sort-Object Name | ForEach-Object {
    psql -h localhost -U fulla_user -d fulla_db -f $_.FullName
}
Get-ChildItem "apps\server\seed\*.sql" | ForEach-Object {
    psql -h localhost -U fulla_user -d fulla_db -f $_.FullName
}

# 3. 构建并运行测试（build.bat 走 Conan + cmake --preset，Release 落到 build/windows-msvc）
.\scripts\backend\build.bat -release
cd build\windows-msvc
$env:FULLA_REDIS_PASSWORD = ""
ctest -V -C Release --output-on-failure
```

---

## 8. 多平台矩阵

多平台 CI 已合并进 `ci.yml` 的 `build-test` Job，通过 `include` 矩阵在同一套可复用工作流（`_build-test.yml`）上跑三个平台。

### Quick Reference

- **Workflow File:** `.github/workflows/ci.yml`（调用 `_build-test.yml`）
- **Platforms:** Linux (ubuntu-24.04), Windows (windows-2022), macOS (macos-14)
- **Trigger:** Push to master, pull requests, manual workflow dispatch
- **Runtime:** ~15-20 minutes cold cache, ~3-5 minutes warm cache per platform

### Platform-Specific Features

每个平台的差异仅通过矩阵输入表达（无复制粘贴的流水线）：

- **Linux:** 系统依赖经 apt 安装；用 Docker 容器跑 PostgreSQL/Redis；执行数据库初始化与命名发布门禁
- **Windows:** Conan 依赖管理，MSVC 2022 编译器；内存存储配置（`use_ci_config`），无外部 DB
- **macOS:** Homebrew（仅 `brew update`）；arm64 构建（`-s arch=armv8`，runner 为 `macos-14`）；内存存储配置

---
