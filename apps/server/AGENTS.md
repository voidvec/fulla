# AGENTS.md — apps/server (fulla-server)

> 本文件是模块级指令，聚焦 `apps/server/` 的职责边界、关键文件路由和开发约束。
> 全局规则（DB 操作、数据访问、ORM 模型、开发流程）见根 `AGENTS.md` 和 `.claude/rules/`。

## 模块职责

**纯装配层**：读取配置、构造 Domain 服务和 Adapter 实现、注入依赖、启动 HTTP 服务器。**不包含任何业务逻辑**。

**职责边界**：
- ✅ Bootstrap 模块（`src/bootstrap/`）：装配 Domain 服务、注册 Controller、设置 CORS/安全头/异常处理
- ✅ Schema 迁移（`src/SchemaManager.*`、`migrations/*.sql`）：数据库 schema 版本管理
- ✅ 配置管理（`config/*.json`）：JSON 基线 + 环境变量覆盖
- ✅ OpenAPI 文档（`openapi.yaml`、`docs/api/openapi.json`）：API 规范
- ✅ 种子数据（`seed/*.sql`）：开发/测试环境初始数据
- ✅ 产品级 Controller（`src/organization/`）：多租户组织管理（跨模块业务）
- ❌ **禁止** Domain 逻辑（业务规则在 `libs/oauth2`、`libs/identity`）
- ❌ **禁止** 直接 DB 访问（通过 `libs/storage-*` 的 Repository 接口）

## 高频变更文件 Top 5

| 排名 | 文件 | 职责 | 开发约束 |
|------|------|------|----------|
| 1 | `openapi.yaml` | OpenAPI 3.0 规范（所有端点定义） | 与 Controller 的 `initApiDocs()` 保持同步；新增端点必须同时更新此文件 |
| 2 | `config/config.bench.json` | 性能测试配置（高并发、连接池调优） | 仅用于 `bench` preset；生产配置在 `config.prod.json` |
| 3 | `config/config.prod.json` | 生产配置（启用 Hodor 限流、HTTPS、严格 CORS） | 必须启用所有安全特性；敏感配置通过环境变量覆盖（`FULLA_DB_*`、`FULLA_REDIS_*`） |
| 4 | `config/config.json` | 开发基线配置（所有环境的公共部分） | 修改需同步到 `config.dev.json`、`config.ci.json`、`config.prod.json` |
| 5 | `CMakeLists.txt` | 构建配置（可执行目标、链接依赖） | 新增 bootstrap 模块需更新 `target_sources`；链接 `fulla::drogon`（传递依赖所有 libs） |

## 关键文件路由

### 主入口（`src/main.cc`）
**纯装配**：`main()` 按顺序调用 bootstrap 模块，不包含业务逻辑。

**启动流程**：
1. 加载配置（`ConfigManager::load()` + 验证）
2. 注册 Controller（`bootstrap::registerAllControllers()`）
3. 设置跨切面（CORS、安全头、异常处理）
4. 注册启动回调（`registerBeginningAdvice`）：
   - 验证 ErrorCatalog 不变式
   - 验证 ResourceScopeRegistry 一致性
   - 注入 Plugin 指针到 Controller/Filter
   - 构造 Identity 服务（`AuthService`、`SessionManager`）
   - 配置 Hodor 限流拒绝响应
5. 初始化 OpenAPI 文档（显式调用每个 Controller 的 `initApiDocs()`）
6. 构建 ResourceScopeRegistry（从 EndpointInfo 集合）
7. 设置数据库迁移（`bootstrap::setupMigrations()`）
8. 启动服务器（`drogon::app().run()`）

**特殊模式**：`--migrate-only` 参数仅运行迁移后退出（Helm pre-install/pre-upgrade hook 使用）。

### Bootstrap 模块（`src/bootstrap/`）

#### ControllerRegistration（`ControllerRegistration.cc/.h`）
- `registerAllControllers()`：注册所有 `AutoCreation=false` 的 Controller
- 必须在 `drogon::app().run()` 之前调用

#### CorsSetup（`CorsSetup.cc/.h`）
- `setupCors()`：从配置读取 CORS 策略（允许的 origin、method、header）
- 生产环境限制具体域名；开发环境允许 `*`

#### SecurityHeaders（`SecurityHeaders.cc/.h`）
- `setupSecurityHeaders()`：添加安全响应头（`X-Content-Type-Options`、`X-Frame-Options`、`Strict-Transport-Security`）
- 生产环境启用 HSTS；开发环境禁用

#### ExceptionHandlerSetup（`ExceptionHandlerSetup.cc/.h`）
- `setupExceptionHandler()`：全局异常捕获，转换为标准 Error Envelope
- 未捕获异常 → HTTP 500 + `INTERNAL_ERROR` 错误码

#### MigrationRunner（`MigrationRunner.cc/.h`）
- `setupMigrations()`：检查并运行 `migrations/*.sql`（按版本号排序）
- `runMigrateOnly()`：仅运行迁移后退出（`--migrate-only` 模式）
- 迁移状态记录在 `schema_migrations` 表

#### IdentityAssembly（`IdentityAssembly.cc/.h`）
- `wireControllerPluginDependencies()`：注入 `OAuth2Plugin` 指针到 Controller/Filter
- `wireIdentityServices()`：构造 `fulla::identity::AuthService` 和 `SessionManager`，注入到 `SessionController`

#### OpenApiSetup（`OpenApiSetup.cc/.h`）
- `setupOpenApi()`：配置 Swagger UI 路由（`/docs/api` → `static/swagger-ui/`）

### Schema 管理（`src/SchemaManager.cc/.h`）
- `SchemaManager`：执行 SQL 迁移脚本，记录迁移状态
- 迁移文件命名：`Vxxx__description.sql`（`xxx` 为三位数版本号）
- 迁移必须幂等（使用 `IF NOT EXISTS`、`ON CONFLICT DO NOTHING`）

### Organization 模块（`src/organization/`）
- `OrganizationController`：多租户组织管理 API（CRUD、成员管理）
- `OrganizationService`：组织业务逻辑（调用 `libs/identity` 的 Repository）
- **注意**：这是唯一包含业务逻辑的 `apps/server` 子目录（跨模块产品级功能）

### 配置（`config/*.json`）

#### 配置文件层次
- `config.json`：开发基线（所有环境的公共部分）
- `config.dev.json`：本地开发（覆盖 `config.json` 的数据库、Redis 连接）
- `config.ci.json`：CI 环境（内存存储、禁用持久化）
- `config.bench.json`：性能测试（高并发、连接池调优）
- `config.prod.json`：生产环境（启用所有安全特性）

#### 环境变量覆盖
- 所有配置项可通过环境变量覆盖：`FULLA_<SECTION>_<KEY>`（如 `FULLA_DB_HOST`、`FULLA_REDIS_PORT`）
- 由 `ConfigManager` 在加载时处理

#### 关键配置段
- `app.log`：日志配置（级别、路径）
- `db_clients`：Postgres 连接池（host、port、dbname、user、password、pool_size）
- `redis_clients`：Redis 连接池（host、port、password）
- `listeners`：HTTP/HTTPS 监听地址
- `plugins`：Drogon 插件配置（Hodor 限流、SecureSSLRedirector）
- `cors`：CORS 策略（allowed_origins、allowed_methods、allowed_headers）

### 迁移脚本（`migrations/*.sql`）
- 按版本号顺序执行（`V001` → `V026`）
- 每个迁移文件包含 `BEGIN;` ... `COMMIT;` 事务
- 关键迁移：
  - `V001`：基础 schema（`schema_migrations` 表）
  - `V002`：OAuth2 核心表（clients、tokens、codes、consents）
  - `V004`：用户表
  - `V005`：RBAC（roles、permissions、user_roles）
  - `V011`：MFA 支持
  - `V018`：WebAuthn
  - `V024`：用户软删除
  - `V025`：审计日志分区

### 种子数据（`seed/*.sql`）
- `dev_admin_user.sql`：开发环境管理员用户
- `dev_admin_console_client.sql`：管理控制台 OAuth2 客户端
- `dev_backend_client.sql`：后端服务 OAuth2 客户端
- `dev_portal_client.sql`：门户前端 OAuth2 客户端
- `bench_users.sql`：性能测试用户数据

### OpenAPI 文档（`openapi.yaml` + `docs/api/openapi.json`）
- `openapi.yaml`：源文件（YAML 格式，人工维护）
- `docs/api/openapi.json`：生成的 JSON 格式（构建时从 YAML 转换）
- 与 Controller 的 `initApiDocs()` 保持同步

### 视图（`views/*.csp`）
- `login.csp`：用户登录页面（CSP 模板）

## 开发约束

### 1. 装配层纪律
- `src/main.cc` 和 `src/bootstrap/` **仅做装配**，不包含业务逻辑。
- 业务逻辑必须在 `libs/oauth2`、`libs/identity`、`libs/common` 的 Domain 层实现。
- `src/organization/` 是唯一例外（跨模块产品级功能）。

### 2. Controller 注册
- 所有 `AutoCreation=false` 的 Controller 必须在 `bootstrap::registerAllControllers()` 中注册。
- 每个 Controller 的 `initApiDocs()` 必须在 `main.cc` 中显式调用（避免循环依赖）。

### 3. 配置管理
- 修改 `config.json` 必须同步到 `config.dev.json`、`config.ci.json`、`config.prod.json`。
- 敏感配置（密码、密钥）通过环境变量覆盖，**禁止**硬编码在配置文件中。
- 生产配置必须启用所有安全特性（Hodor 限流、HTTPS、严格 CORS、HSTS）。

### 4. Schema 迁移
- 新增迁移文件命名：`Vxxx__description.sql`（`xxx` 为当前最大版本号 +1）
- 迁移必须幂等（使用 `IF NOT EXISTS`、`ON CONFLICT DO NOTHING`）
- 迁移文件包含 `BEGIN;` ... `COMMIT;` 事务
- 修改已有迁移文件**禁止**（会破坏已部署环境的迁移状态）

### 5. 依赖方向
- ✅ `apps/server` → `libs/drogon`（传递依赖所有 libs）
- ✅ `apps/server` → `libs/common`、`libs/oauth2`、`libs/identity`（直接引用 Domain 类型）
- ❌ `libs/*` → `apps/server`（libs 禁止依赖 apps）

### 6. 启动顺序
`main.cc` 的启动顺序严格固定：
1. 加载配置
2. 注册 Controller
3. 设置跨切面（CORS、安全头、异常处理）
4. 注册 `registerBeginningAdvice` 回调（按注册顺序执行）
5. 初始化 OpenAPI 文档
6. 构建 ResourceScopeRegistry
7. 设置数据库迁移
8. 启动服务器

**禁止**调整顺序（会导致依赖未初始化的组件被访问）。

### 7. 测试
- `apps/server` 无独立单元测试（逻辑在 libs 中测试）。
- 集成测试在 `tests/integration/`，使用真实 Postgres 实例（Docker Compose）。
- E2E 测试在 `tests/e2e-backend/`，验证完整 OAuth2 流程。

### 8. 构建与运行
- 构建：`./manage.sh build`（Linux/macOS）或 `./manage.ps1 build`（Windows）
- 运行：`./manage.sh run`（启动开发服务器）
- 测试：`./manage.sh test`（运行所有测试）
- 迁移：`./manage.sh migrate`（手动运行迁移）

## 相关规则

- 根 `AGENTS.md`：全局规则索引
- `.claude/rules/db-operations.md`：DB 操作规则（async callback + Mapper + Criteria）
- `.claude/rules/data-access.md`：存储层触发器
- `.claude/rules/orm-models.md`：ORM 模型禁止手改
- `.claude/rules/dev-workflow.md`：构建/测试命令（`./manage.sh`、`./manage.ps1`）
