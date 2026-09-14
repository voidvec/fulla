# Fulla — 高性能开源 IAM 核心（C++17）

[English](README.md)

![CI](https://github.com/voidvec/fulla/actions/workflows/ci.yml/badge.svg)
![Security](https://github.com/voidvec/fulla/actions/workflows/security.yml/badge.svg)
[![Release](https://img.shields.io/github/v/release/voidvec/fulla)](https://github.com/voidvec/fulla/releases/latest)
[![PyPI](https://img.shields.io/pypi/v/fulla-oauth2.svg)](https://pypi.org/project/fulla-oauth2/)
[![Go Reference](https://pkg.go.dev/badge/github.com/voidvec/fulla/clients/go.svg)](https://pkg.go.dev/github.com/voidvec/fulla/clients/go)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)
![Conan](https://img.shields.io/badge/Conan-2.x-6699CB.svg)
![License](https://img.shields.io/badge/license-AGPL_v3-blue.svg)
[![Benchmark](https://img.shields.io/badge/benchmark-5%2F5%20scenarios%20lead-brightgreen)](benchmarks/competitors/results/COMPARISON.md)

Fulla 是以 C++17 构建的**高性能开源身份与访问管理（IAM）核心**：生产级 OAuth2.0/OIDC 授权服务器（RFC 6749/7662/7009/8414），完整覆盖用户认证、MFA、WebAuthn、RBAC 与多租户——既可作为**开箱即用的产品**（Docker/Helm）部署，也可作为**可嵌入的 C++ SDK**（`find_package(fulla-*)`）集成。包含管理后台、用户前端与完整的测试体系。

> **路线图 · 开源核心：** 开源核心是可选商业增强模块（企业集成与支持服务，规划中）的底座。运行一套完整 IAM 所需的一切，现在且永远以 AGPL-3.0 许可开源。

---

## 能力地图（Capability Map）

产品按领域能做什么。深入文档在 `docs/` 下（会话管理、令牌生命周期、多租户三篇深度文档在文档路线图中）。

| 领域 | 能力 | 深入文档 |
|------|------|----------|
| **认证** | 登录/注册、邮箱验证、密码重置、TOTP MFA、WebAuthn (FIDO2)、Google/微信社交登录、渐进式账户锁定 | [安全架构](https://fulla.dev/zh-CN/docs/architecture/security-architecture) |
| **授权（OAuth2/OIDC）** | 授权码 + PKCE、client-credentials、刷新轮换、设备流、动态客户端注册、用户同意、内省、吊销、OIDC discovery/JWKS/UserInfo、含前/后向通道登出的 end-session | [架构总览](https://fulla.dev/zh-CN/docs/architecture/architecture-overview) |
| **访问控制** | RBAC（内置 admin/user + 自定义角色）、细粒度 scope、DB 驱动的资源 scope 注册表、三重校验（客户端限制 + 角色 + 同意） | [RBAC 指南](https://fulla.dev/zh-CN/docs/domains/rbac-guide) |
| **令牌生命周期** | 签发、TTL 约束保留、基于家族的刷新轮换、按令牌/客户端/用户吊销、延迟双删的缓存旁路失效 | — |
| **多租户** | 组织、组织级客户端与用户、租户感知的管理 | — |
| **可观测性** | Prometheus 指标、结构化审计日志（登录/令牌/密码事件）、健康探针（live/ready） | [可观测性](https://fulla.dev/zh-CN/docs/operate/observability) |
| **运维** | Docker Compose / Helm 部署、cosign 签名多架构镜像、SBOM、配置文件 + 环境变量驱动配置 | [生产部署](https://fulla.dev/zh-CN/docs/operate/deployment) |

## 模块地图（Module Map）

[SDK 分层](#sdk-分层架构)图背后的 8 个 CMake 包——各自负责什么、公共头在哪（`libs/<name>/include/fulla/…`）：

| 包 | 职责 | 备注 |
|----|------|------|
| `fulla::common` | 共享内核：值对象、Result、错误目录、端口（时钟/加密/指标/邮件/审计） | 零 Drogon 依赖 |
| `fulla::oauth2` | OAuth2/OIDC 引擎：授权流、令牌服务、PKCE、JWK 管理、scope 决策引擎 | 协议核心，存储无关 |
| `fulla::identity` | 认证域：用户、会话、MFA、WebAuthn、社交账号、角色提供 | 构建期可选 |
| `fulla::drogon` | Drogon 适配层：HTTP 控制器、过滤器、视图、OAuth2 插件、管理服务 | 服务器宿主链接的包 |
| `fulla::storage::memory` | 内存仓储实现（测试、嵌入式单进程场景） | 零外部依赖 |
| `fulla::storage::postgres` | PostgreSQL 仓储 + Drogon ORM 模型、schema 管理 | PostgreSQL 14+ |
| `fulla::storage::redis` | Redis 缓存旁路层（延迟双删失效） | 包装任意仓储层 |
| `fulla::storage` 契约 | `IClientRepository` / `IGrantRepository` / `ITokenRepository` / `IConsentRepository` | 契约测试套件跨实现验证 |

通过 Conan/CMake 选项（`with_identity` / `with_social` / `with_webauthn`）收缩功能面，SDK 消费方只拉取所需部分。

---

## 项目架构

```
fulla/
├── apps/server/        # 授权服务器后端（Drogon C++ 框架）
├── libs/               # SDK 库包（fulla::common/oauth2/identity/storage-*/drogon）
├── frontends/admin/    # 管理后台前端（Vue 3 + TailwindCSS）
├── frontends/user/     # 用户端前端（Vue 3 + Pinia + TailwindCSS）
├── examples/           # SDK 消费示例（find_package 冒烟宿主）
├── deploy/             # Docker Compose、Helm chart、nginx、可观测性
├── tests/              # 后端测试套件（单元 / 集成 / 契约）
├── scripts/            # 构建、测试、运维脚本
└── docs/               # 项目文档
```

### SDK 分层架构

后端拆分为 8 个 CMake 包，依赖方向受强制约束（Domain 层永不依赖 Drogon，由 CI 中的 `tools/arch-guard` 把关）。箭头语义为「依赖于」：

```mermaid
graph TD
    server["fulla-server<br/>(apps/server)"] --> drogon
    drogon["fulla::drogon<br/>插件 · 控制器 · 过滤器 · 视图"] --> oauth2
    drogon --> identity
    drogon --> memory
    drogon --> redis
    drogon --> postgres
    memory["fulla::storage::memory"] --> oauth2
    redis["fulla::storage::redis"] --> oauth2
    postgres["fulla::storage::postgres<br/>(ORM 模型)"] --> identity
    oauth2["fulla::oauth2<br/>OAuth2/OIDC 引擎"] --> common
    identity["fulla::identity<br/>认证 · MFA · WebAuthn · RBAC"] --> common
    common["fulla::common<br/>共享内核 · 端口接口"]
```

可选特性面由 Conan/CMake 选项门控（`with_identity` / `with_social` / `with_webauthn`），SDK 消费方可据此收缩依赖表面。

### 技术栈

| 层级 | 技术 |
|------|------|
| 后端框架 | Drogon (C++17) |
| 数据库 | PostgreSQL 14+ |
| 缓存 | Redis 7+ |
| 管理后台 | Vue 3 + Vite + Pinia + TailwindCSS |
| 用户前端 | Vue 3 + Vite |
| 测试 | CTest (C++) + Playwright (E2E) + PowerShell (API) |
| 监控 | Prometheus + 审计日志 |
| 部署 | Docker Compose / Nginx |

---

## 功能模块

### OAuth2/OIDC 核心协议

| 功能 | 标准 | 端点 |
|------|------|------|
| 授权码流程 + PKCE | RFC 6749 / RFC 7636 | `/oauth2/authorize`、`/oauth2/login`、`/oauth2/token` |
| 客户端凭证模式 | RFC 6749 | `/oauth2/token` (grant_type=client_credentials) |
| Token 刷新 | RFC 6749 | `/oauth2/token` (grant_type=refresh_token) |
| Token 内省 | RFC 7662 | `/oauth2/introspect` |
| Token 撤销 | RFC 7009 | `/oauth2/revoke` |
| OIDC Discovery | RFC 8414 | `/.well-known/openid-configuration` |
| JWKS | RFC 7517 | `/.well-known/jwks.json` |
| UserInfo | OIDC Core | `/oauth2/userinfo` |
| 用户同意 | OAuth2 | `/oauth2/consent` |
| 设备授权流 | RFC 8628 | `/oauth2/device_authorization` |
| 动态客户端注册 | RFC 7591 | `/oauth2/register` |

### 用户认证与安全

| 功能 | 端点 |
|------|------|
| 用户注册 | `POST /api/register` |
| 密码重置 | `/api/password-reset/request`、`/api/password-reset/confirm` |
| 邮箱验证 | `/api/verify-email`、`/api/verify-email/resend` |
| MFA (TOTP) | `/api/me/mfa/setup`、`/api/me/mfa/verify`、`/api/me/mfa/disable` |
| WebAuthn (FIDO2) | `/api/me/webauthn/register/*`、`/oauth2/webauthn/authenticate/*` |
| 外部登录 (Google) | `/api/google/login` |
| 外部登录 (微信) | `/api/wechat/login` |
| 账号锁定保护 | 渐进式锁定（5/10/15/20次失败递增） |

### 用户自助服务

| 功能 | 端点 |
|------|------|
| 个人资料 | `GET /api/me` |
| 修改密码 | `PUT /api/me/password` |
| 已授权应用管理 | `GET/DELETE /api/me/authorized-apps` |
| 注销账号 | `DELETE /api/me` |

### Admin 管理后台 (frontends/admin)

| 模块 | 功能 |
|------|------|
| 仪表盘 | 用户数、应用数、活跃Token数、失败登录统计 |
| 应用管理 | Client CRUD、Secret 重置、Scope 分配、Grant Type 配置 |
| 用户管理 | 用户列表/详情、角色分配、禁用/启用、锁定状态查看 |
| 角色管理 | 角色 CRUD（保护内置角色 admin/user） |
| Scope 管理 | Scope CRUD（保护内置 Scope openid/profile/email/admin） |
| Token 管理 | Token 列表、按客户端/用户撤销、单个撤销 |
| 组织管理 | 多租户组织 CRUD |
| 审计日志 | 分页查看、按事件类型/结果筛选 |
| OIDC 密钥 | 签名密钥信息查看 |
| 系统设置 | 健康状态监控 |

### RBAC 权限系统

- 基于角色的访问控制（admin / user / 自定义角色）
- URL 模式匹配的权限检查（`/api/admin/.*` → admin 角色）
- 三重 Scope 权限控制（Client 限制 + Role 校验 + Consent 检查）

### 可观测性

- Prometheus 指标导出 (`/metrics`)
- 结构化审计日志（登录、Token 签发/撤销、密码变更等）
- 健康检查端点 (`/health`、`/health/live`、`/health/ready`)

---

## 性能

与 Keycloak 26.7.1 / Ory Hydra v26.2.0 / Zitadel v4.17.1 的同环境对比（同一台空闲主机、
同 session 串行执行、各家官方推荐配置、同一 PostgreSQL 17 后端、同一 wrk 阶梯 2→128）。
Fulla 使用文档化基准档（池 64/64、cache on、`auto_batch`、`reuse_port`、opt-in LTO
构建、TTL=30 留存有界 session）。**五个对比场景全部领先**（2026-08-23 刷新，
[完整报告与方法论](benchmarks/competitors/results/COMPARISON.md)）：

| 场景 | Fulla | 对亚军倍数 |
|---|---|---|
| discovery（`/.well-known/openid-configuration`） | 87,499 QPS | 2.1x Keycloak |
| client_credentials 签发 | 14,438 QPS | 2.6x Keycloak |
| token 内省（introspect） | 22,458 QPS | 2.0x Ory Hydra |
| refresh_token 轮换 | 5,506 QPS | 1.9x Keycloak |
| userinfo | 49,302 QPS | 1.5x Keycloak |
| 冷启动（全栈就绪首个 200） | 1.26 s | 快 14.5x（vs Keycloak） |

诚实限定：测于 WSL2 8 vCPU / 16 GB（数字是下限，非裸机）；"轻量"叙事适用的是
**SDK 嵌入口径（2.5 MB peak working set）**而非容器全栈口径；本机逐段 P99 抖动由宿主
噪声主导，不作为差异化主张（见报告 GC 节）。

### 如何复现

```bash
# 一条命令跑四产品，同 session 串行（需 Docker + 空闲主机；端到端约 3 小时）：
bash benchmarks/competitors/run-comparison.sh --fresh
# 从入仓结果 JSON 重新生成报告（无手填数字）：
python3 benchmarks/reporting/gen-comparison.py
```

方法论与公平性偏离项（各家配置出处、对齐了什么没对齐什么）：
[competitor-benchmark-design.md](https://fulla.dev/zh-CN/docs/benchmark/competitor-benchmark-design)。
Fulla 侧场景细节：[benchmarks/README.md](benchmarks/README.md)。

---

## 快速开始

### 路径 A — Docker Compose（推荐用于评估）

```bash
docker compose -f deploy/docker/docker-compose.yml up -d --build
```

- 用户前端：`http://localhost:8080`
- 管理后台：`http://localhost:8081`
- 后端 API：`http://localhost:5555`

> **仅限开发的凭证（#112）：** 该 compose 文件内置弱口令（数据库
> `123456`、Redis `redis_secret_pass`、fulla-portal `123456`，以及种子账号
> `admin`/`admin`，自 #103 起以 PBKDF2 哈希存储），种子管理员只用于评估。PostgreSQL（127.0.0.1:5433）与
> Redis（127.0.0.1:6380）特意只绑定回环地址。任何超出本地评估的用途请使用
> [`docker-compose.prod.yml`](deploy/docker/docker-compose.prod.yml) +
> `.env.docker` —— 生产校验器（`FULLA_ENV=production`）启动时会拒绝这些默认值。

### 路径 B — 源码构建

标准构建流程为 Conan 2 + CMake preset（与 CI 完全一致）：

```bash
# 1. 解析锁定的依赖（将 toolchain 写入 preset 对应的构建目录）
conan install . --output-folder=build/linux-release --build=missing \
  -s build_type=Release -s compiler.cppstd=17

# 2. 配置 + 构建
#    可用 preset：linux-release / windows-msvc / macos-arm64（另有 -debug / -asan / -tsan 变体）
cmake --preset linux-release
cmake --build --preset linux-release

# 3. 运行后端测试套件
ctest --test-dir build/linux-release --output-on-failure
```

`manage.ps1`（Windows）与 `manage.sh`（Linux/macOS）封装了同一流程，作为便捷命令使用，如 `.\manage.ps1 build-backend`。

本地运行全栈（后端需要 PostgreSQL + Redis）：

```powershell
# 后端
cd apps\server
..\..\build\windows-msvc\apps\server\Release\fulla-server.exe

# 管理后台 — http://localhost:5174/admin/
cd frontends\admin && npm install && npm run dev

# 用户前端 — http://localhost:5173
cd frontends\user && npm install && npm run dev
```

### 路径 C — 以 SDK 方式集成

通过 `find_package` 将 Fulla 嵌入自己的 C++ 宿主（SDK 包取自 [Releases](https://github.com/voidvec/fulla/releases)，或从源码 `cmake --install`）：

```cmake
# 全栈：一个包拉取完整闭包（引擎 + Drogon 插件/控制器）
find_package(fulla-drogon CONFIG REQUIRED)
target_link_libraries(my-host PRIVATE fulla::drogon)

# 或仅取引擎面（无 Drogon 依赖）：
find_package(fulla-oauth2 CONFIG REQUIRED)
find_package(fulla-storage-memory CONFIG REQUIRED)
target_link_libraries(my-engine PRIVATE fulla::oauth2 fulla::storage::memory)
```

> v1.x 对公共头（`include/fulla/**`）承诺**源码级 SemVer**（CI 中 api-diff 门禁强制），不承诺二进制 ABI。第三方依赖请用仓库的 `conanfile.py` + `conan.lock` 解析。详见 [SDK 集成指南](https://fulla.dev/zh-CN/docs/sdk/sdk-integration-guide) · [SDK 运行时契约](https://fulla.dev/zh-CN/docs/sdk/sdk-runtime-contract)；参考消费方：[`examples/full-stack-host`](examples/full-stack-host)、[`examples/third-party-host`](examples/third-party-host)（均由 CI 持续验证）。

### 路径 D — 客户端 SDK（Python / Go）

非 C++ 服务通过 HTTP API 使用 Fulla：类型化生成客户端 + 手写 auth 层（token 生命周期绝不模板化生成）：

```python
# Python（发行名 fulla-oauth2，导入名 fulla）
from fulla import m2m_client

client = m2m_client("http://localhost:5555", "backend-svc", "…", scopes=["tokens:read"])
```

```go
// Go（github.com/voidvec/fulla/clients/go）
client, _ := af.NewM2MClient(ctx, "http://localhost:5555", "backend-svc", "…", []string{"tokens:read"})
```

两者均从单一源 OpenAPI spec（`apps/server/openapi.yaml`）生成，CI 有新鲜度漂移门。见 [`clients/python`](clients/python) 与 [`clients/go`](clients/go)。包管理器发布（PyPI / Go module proxy）随首个携带它们的 tag 版本启动。

### 默认账号

| 用户名 | 密码 | 角色 |
|--------|------|------|
| admin | *（首次启动自动生成——仅打印一次在服务日志中，或用 `FULLA_BOOTSTRAP_ADMIN_PASSWORD` 指定）* | admin |

---

## 部署

| 目标 | 入口 | 说明 |
|------|------|------|
| Docker Compose（开发） | `deploy/docker/docker-compose.yml` | 全栈 + PostgreSQL + Redis，一条命令拉起 |
| Docker Compose（生产） | `deploy/docker/docker-compose.prod.yml` | TLS/nginx，env 文件驱动的密钥配置 |
| Kubernetes（Helm） | `deploy/helm/fulla` | values 驱动配置；数据库 Schema 迁移以 Helm hook Job 执行 |

```bash
helm install fulla deploy/helm/fulla -f my-values.yaml
```

完整流程：[生产部署指南](https://fulla.dev/zh-CN/docs/operate/deployment) · [Windows / Docker Desktop](https://fulla.dev/zh-CN/docs/operate/deployment-windows-docker-desktop) · [安全清单](https://fulla.dev/zh-CN/docs/architecture/security-architecture)

---

## 发布与供应链安全

发布由 SemVer 标签（`vX.Y.Z`）触发 [`release.yml`](.github/workflows/release.yml) 产出：

- **SDK 包** — `fulla-sdk-<ver>-linux-x86_64.tar.gz`（8 个静态库 + 头文件 + CMake 包配置）附 `.sha256` 校验和，挂在 GitHub Release 附件。
- **容器镜像** — 多架构（amd64 + arm64）发布到 GHCR：`ghcr.io/voidvec/fulla-{backend,frontend,admin}:<ver>`。
- **签名** — 镜像 manifest 按 digest 用 cosign 签名（keyless，GitHub OIDC）。
- **SBOM** — 每个镜像及源码树的 SPDX JSON（syft 生成），附在 Release 中。

部署前验证：

```bash
# 镜像签名
cosign verify ghcr.io/voidvec/fulla-backend:<version> \
  --certificate-identity-regexp 'github.com/voidvec/.+/.github/workflows/release.yml' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com

# SDK 包完整性
sha256sum -c fulla-sdk-<version>-linux-x86_64.tar.gz.sha256
```

---

## 测试

### 后端 API 测试

```powershell
# Admin API 全量测试
.\scripts\backend\test-admin-endpoints.ps1

# OAuth2 核心流程测试
.\scripts\backend\test-oauth2-endpoints.ps1
```

### 前端 E2E 测试

```powershell
cd frontends\admin
npx playwright test              # 全量运行
npx playwright test --ui         # UI 模式调试
npx playwright test --headed     # 有头浏览器模式
```

### C++ 单元测试

```powershell
cd build\windows-msvc
ctest --output-on-failure
```

### 测试覆盖统计

| 测试类型 | 覆盖范围 |
|----------|----------|
| C++ 单元/集成测试 (CTest) | SDK 库、领域服务、存储适配器 |
| Admin API (PowerShell) | 全部 Admin 端点 + Organization |
| OAuth2 Core (PowerShell) | 认证流程、Token 管理、用户服务 |
| 前端 E2E (Playwright) | 管理后台与用户前端的页面和交互 |

---

## API 文档

- **OpenAPI 规范**：[openapi.yaml](apps/server/openapi.yaml)
- **Swagger UI**：`http://localhost:5555/docs/api`（需部署 Swagger UI 静态文件）
- **E2E 测试指南**：[Admin E2E 方法论](https://fulla.dev/zh-CN/docs/contribute/admin-e2e-testing-guide)

---

## 项目文档

**在线文档站：[fulla.dev/zh-CN](https://fulla.dev/zh-CN)** —— 由本仓库 `docs/` 目录（英文正本）及其
`website/i18n/zh-CN/` 中文译本直接构建，随 master 持续更新；站点右上角可切换中英文。

**评估选型** — [架构总览](https://fulla.dev/zh-CN/docs/architecture/architecture-overview) · [安全架构](https://fulla.dev/zh-CN/docs/architecture/security-architecture) · [RBAC 权限](https://fulla.dev/zh-CN/docs/domains/rbac-guide) · [性能对比](benchmarks/competitors/results/COMPARISON.md)

**SDK 集成** — [SDK 集成指南](https://fulla.dev/zh-CN/docs/sdk/sdk-integration-guide) · [SDK 运行时契约](https://fulla.dev/zh-CN/docs/sdk/sdk-runtime-contract) · [API 参考](https://fulla.dev/zh-CN/docs/domains/api-reference)

**运维部署** — [生产部署指南](https://fulla.dev/zh-CN/docs/operate/deployment) · [配置指南](https://fulla.dev/zh-CN/docs/operate/configuration-guide) · [可观测性](https://fulla.dev/zh-CN/docs/operate/observability) · [账号锁定机制](https://fulla.dev/zh-CN/docs/operate/account-lockout)

**参与贡献** — [CONTRIBUTING.md](CONTRIBUTING.md) · [测试指南](https://fulla.dev/zh-CN/docs/contribute/testing-guide) · [CI/CD 流水线](https://fulla.dev/zh-CN/docs/contribute/ci-cd-guide)

完整索引：[docs/README.md](https://fulla.dev/zh-CN/docs/intro)

---

## 系统要求

| 组件 | 最低版本 |
|------|----------|
| C++ 编译器 | C++17 (MSVC 2019+ / GCC 9+ / Clang 10+) |
| CMake | 3.21+ |
| PostgreSQL | 14+ |
| Redis | 7+ |
| Node.js | 18+ |
| Docker | 24+（可选） |

---

## 贡献与安全

- 欢迎贡献 — 构建、测试、提交规范见 [CONTRIBUTING.md](CONTRIBUTING.md)。
- 报告安全漏洞请遵循 [SECURITY.md](SECURITY.md) — 请**勿**直接提交公开 issue。

---

## 许可证

AGPL-3.0 — 详见 [LICENSE](LICENSE)。历史版本(v1.0.0)仍为 MIT。

---

**项目状态**：生产就绪 | **版本**：v1.3.0
