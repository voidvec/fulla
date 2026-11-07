# AGENTS.md — fulla 项目指令

> 本文件是 ZCode 的项目级指令文件（`<repo>/AGENTS.md`）。
>
> **项目规则的权威源头是 `.claude/rules/`**（git 跟踪）。本文件只做**索引和导航**，
> 不复述规则正文 —— 这样规则只有一份，不会与 `.claude/rules/` 漂移。当本文件与
> `.claude/rules/` 冲突时，以 `.claude/rules/` 为准。

## 规则索引

| 规则 | 文件 | 适用路径 | 一句话摘要 |
|---|---|---|---|
| DB 操作 | [`.claude/rules/db-operations.md`](.claude/rules/db-operations.md) | `libs/**`、`apps/server/**` | DB 必须 async callback + Mapper + Criteria 三件套；raw SQL 仅 6 种豁免；每个 `Mapper<...>(db)` 构造独立 try-catch；catch 必须 `(*sharedCb)(errorResult)` 回调失败，禁止仅 LOG_ERROR 后 return |
| 数据访问 | [`.claude/rules/data-access.md`](.claude/rules/data-access.md) | `libs/storage-*/**` | 指向 db-operations 的存储层触发器（规则同上） |
| ORM 模型 | [`.claude/rules/orm-models.md`](.claude/rules/orm-models.md) | `**/models/**` | `models/` 下的 ORM 类由 `drogon_ctl` 从 schema 生成，**禁止手改**；要改模型就改 schema 再 `/orm-gen` |
| 开发流程 | [`.claude/rules/dev-workflow.md`](.claude/rules/dev-workflow.md) | `apps/server/**`、`frontends/**` | 优先 `./manage.sh`（Linux/macOS）/ `./manage.ps1`（Windows）；`/build-and-test` 等 skill 是详解，不是首选入口 |
| CI 同步点 | [`.claude/rules/sync-points.md`](.claude/rules/sync-points.md) | 错误码/端点/迁移/公共头/golden 相关路径 | 错误码 8 处、端点 5 处同步硬清单，漏一处 CI 必挂 |
| 前后端契约 | [`.claude/rules/frontend-contract.md`](.claude/rules/frontend-contract.md) | `frontends/**`、`apps/server/openapi.yaml` | 实测契约事实（PKCE 强制、token 内存态、端点口径），写前端/改契约前核对 |
| 异步补充坑 | [`.claude/rules/async-cpp-gotchas.md`](.claude/rules/async-cpp-gotchas.md) | `libs/**`、`apps/server/**`、`tests/**` | db-operations 之外的实测坑（Session::insert 不覆盖、double-move、`::drogon::` 遮蔽） |

## 模块级 AGENTS.md

热点模块的嵌套指令文件，聚焦该模块的职责边界、关键文件路由和开发约束：

| 模块 | 文件 | 职责 | 高频变更热点（次数略，见 git log） |
|---|---|---|---|
| Drogon 绑定层 | [`libs/drogon/AGENTS.md`](libs/drogon/AGENTS.md) | Adapter 层 Drogon 适配包（Controllers、Filters、Plugin、Adapters） | `src/controllers/SessionController.cc` |
| Postgres 存储层 | [`libs/storage-postgres/AGENTS.md`](libs/storage-postgres/AGENTS.md) | Adapter 层 Postgres Repository 实现（ORM 模型 + Repository 接口） | `src/PostgresIdentityRepository.cc` |
| 服务器装配层 | [`apps/server/AGENTS.md`](apps/server/AGENTS.md) | 纯装配层（Bootstrap、配置、迁移、OpenAPI） | `openapi.yaml` |

## 异步编程与 DB 访问

**完整规则见 [`.claude/rules/db-operations.md`](.claude/rules/db-operations.md)，本文件不重复。**
涉及异步/DB 代码时务必先读那份文件。关键词速查：

- 接口选择：async callback（首选）→ `Mapper::findBy`-with-future（受限，仅当真需要同步结果）→ `CoroMapper`（禁止）。
- Callback 生命周期：`std::make_shared<CallbackType>(std::move(cb))`，按值捕获进每层 lambda。
- Mapper 构造异常防护：每个 `Mapper<...>(dbClient)` 构造都要独立 `try/catch`，包括嵌套在异步回调内的 —— 外层保护不到内层异步回调，未捕获会逃逸到 Drogon 事件循环导致 SIGABRT。
- JOIN 禁用：拆成多个查询，或用 `Criteria::In(...)`。
- raw SQL 仅 6 种豁免：DDL / `UPDATE ... RETURNING` / 文档化的批量操作 / `INSERT ... ON CONFLICT` / `SELECT 1` 探活 / 显式事务 `COMMIT`。

## `[this]` 捕获（易混淆点，单独说明）

不同层级约定不同，不要混淆：

- **Domain 服务层**（`libs/oauth2`、`libs/identity` 的 service）：**禁止** `[this]` 和 `[&var]`，必须 `auto self = shared_from_this()` 后捕获 `self`。见 db-operations.md。
- **Controller 层**（`libs/drogon/.../controllers`）：`[this]` **允许且普遍**。Drogon `HttpController<T,false>` 是进程级单例，由 Drogon 用裸指针管理，存活整个进程；`shared_from_this()` 不适用（不继承 `enable_shared_from_this`）。范例注释见 `TokenEndpointController.cc:2165-2170`（以 `P1 #4` 开头的注释块）。

> 旧版 `TECH_SPECS.md` 曾把 `[this]` 列为全局禁令，与 controller 层实践冲突，已在本次精简中移除该条。

## 跟踪类 issue 创建规范

用 `gh`/API 创建跟踪类 issue（审计发现、评审遗留、技术债登记）时，**英文**填写并套用
[`.github/ISSUE_TEMPLATE/tracking.md`](.github/ISSUE_TEMPLATE/tracking.md) 的骨架
（Problem / Evidence / Suggested fix / Acceptance criteria / Out of scope）——
issue forms 只在网页新建流程生效，CLI 会绕过，故以此模板对齐结构。
面向用户的 bug/feature 仍走网页表单模板，不要用 tracking 骨架。

## 各 AI 工具目录的角色

本仓库的规则与工作流资产以 `.claude/` 为**唯一权威源**（git 跟踪）：

| 目录 | 角色 |
|---|---|
| `.claude/` | **规则源头**（`rules/`）、agents、commands、skills、hooks、settings —— git 跟踪，权威 |
| `.codebuddy/`、`.qoder/`、`.kiro/`、`.zcode/`、`.workbuddy/` | 各 AI 工具的**本地工作区**（各自的 skills 镜像/plans/记忆）——不入库（.gitignore 忽略，磁盘保留） |

**改规则就改 `.claude/rules/`**（唯一副本）。如需在其它工具里生效，由各工具目录的本地副本自行同步，不再要求版本库维护多份一致镜像。不要在 AGENTS.md 或 TECH_SPECS.md 里复制规则正文。入库范围的标准见 [docs/documentation-governance.md](docs/documentation-governance.md)（入库三判据 + 本地维护区约定）。

## 其它项目文档

- `TECH_SPECS.md` — 架构、代码质量、安全等**非异步/非DB**的项目契约（异步与 DB 规则已迁移到 `.claude/rules/`，TECH_SPECS 只保留引用指针）。
- `docs/` — 架构概览、SDK 契约、测试指南、运维文档等。
- `README.md` — Quick Start、构建/运行/测试命令。
