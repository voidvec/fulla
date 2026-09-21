# AGENTS.md — libs/drogon (fulla::drogon)

> 本文件是模块级指令，聚焦 `libs/drogon/` 的职责边界、关键文件路由和开发约束。
> 全局规则（DB 操作、数据访问、ORM 模型、开发流程）见根 `AGENTS.md` 和 `.claude/rules/`。

## 模块职责

**Adapter 层 Drogon 绑定包**：将 Domain 层（`libs/oauth2`、`libs/identity`、`libs/common`）的端口接口适配到 Drogon HTTP 框架。

**职责边界**：
- ✅ Controllers（OAuth2 端点、管理 API、自助服务）
- ✅ Filters（认证、授权、请求验证）
- ✅ Drogon Plugin（装配器，构造 Domain 服务并注入依赖）
- ✅ Adapters（加密、日志、审计、指标、HTTP 客户端、UUID 生成）
- ✅ Validation 规则引擎（`fulla::drogon::validation::*`）
- ✅ Error 处理（ErrorResponder、ErrorHandler）
- ✅ Observability（审计日志、指标、OpenAPI 生成）
- ❌ **禁止** Domain 逻辑（业务规则在 `libs/oauth2`、`libs/identity`）
- ❌ **禁止** 直接 DB 访问（通过 `libs/storage-*` 的 Repository 接口）

## 高频变更文件 Top 5

| 排名 | 文件 | 职责 | 开发约束 |
|------|------|------|----------|
| 1 | `src/controllers/SessionController.cc` | 用户会话管理（登录、登出、会话刷新） | `[this]` 允许；调用 `fulla::identity::SessionManager`；禁止直接构造 Mapper |
| 2 | `src/controllers/GitHubController.cc` | GitHub OAuth 社交登录流程 | `#ifdef WITH_SOCIAL` 守卫；调用 `fulla::identity::SocialLoginService`；禁止硬编码 client_id/secret |
| 3 | `src/plugin/OAuth2Plugin.cc` | Drogon Plugin 装配器（构造 RepositoryBundle、Domain 服务、速率限制器） | 全局命名空间 `::OAuth2Plugin`（config.json 反射依赖）；`initAndStart()` 按顺序构造：配置 → 存储 → 服务 → 速率限制 |
| 4 | `src/controllers/TokenEndpointController.cc` | OAuth2 Token 端点（授权码换 token、刷新 token、客户端凭证） | `[this]` 允许；调用 `fulla::oauth2::TokenService`；PKCE 验证在 Domain 层完成 |
| 5 | `src/controllers/MfaController.cc` | MFA 挑战/验证/绑定（TOTP、WebAuthn） | `#ifdef WITH_WEBAUTHN` 守卫部分；调用 `fulla::identity::MfaService`；TOTP 密钥生成在 `utils/TotpUtils.h` |

## 关键文件路由

### Controllers（`src/controllers/` + `include/fulla/drogon/controllers/`）
- **OAuth2 核心端点**：`AuthorizationEndpointController`、`TokenEndpointController`、`DiscoveryController`
- **管理 API**：`UserAdminController`、`ClientAdminController`、`TokenAdminController`、`RoleScopeAdminController`、`AuditController`
- **自助服务**：`UserSelfServiceController`、`PasswordResetController`、`EmailVerificationController`
- **社交登录**：`GoogleController`、`WeChatController`、`GitHubController`（`WITH_SOCIAL` 守卫）
- **WebAuthn**：`WebAuthnController`（`WITH_WEBAUTHN` 守卫）
- **设备授权**：`DeviceAuthController`

### Filters（`src/filters/` + `include/fulla/drogon/filters/`）
- `AuthorizationFilter`：Bearer token 解析 + 基础认证
- `OAuth2AuthFilter`：OAuth2 scope 验证（调用 `fulla::drogon::authz::ScopeResolver`）
- `RequestValidationFilter`：请求体 schema 验证（调用 `fulla::drogon::validation::RuleEngine`）

### Plugin（`src/plugin/` + `include/fulla/drogon/plugin/`）
- `OAuth2Plugin.cc`：**唯一**的 Drogon Plugin 实现，装配所有 Domain 服务和 Repository
- `OAuth2CleanupService.cc`：定时清理过期 token、授权码、设备码

### Adapters（`src/adapters/` + `include/fulla/drogon/adapters/`）
- 实现 `libs/common` 和 `libs/identity` 的端口接口：`DrogonLogger`、`DrogonAuditSink`、`DrogonMetrics`、`OpenSslCryptoProvider`、`OpenSslUuidGenerator`、`SystemClock`
- HTTP 客户端：`DrogonOAuthHttpClient`（实现 `fulla::identity::IOAuthHttpClient`）
- 存储适配：`StorageRoleProvider`、`StorageSubjectResolver`

### Services（`src/services/` + `include/fulla/drogon/services/`）
- `IdentityService`：Thin forwarder，构造 `fulla::identity::AuthService` 和 `SessionManager`
- `ClientRegistrationService`、`DeviceCodeService`、`EmailVerificationService`、`PasswordResetService`

### Utils（`src/utils/` + `include/fulla/drogon/utils/`）
- `CryptoUtils`：OpenSSL 封装（哈希、随机数生成）
- `PasswordHasher`：Argon2id 密码哈希
- `ScopeChecker`：scope 字符串解析与匹配
- `SubjectGenerator`：用户 sub 生成（UUID → 公开标识）
- `TotpUtils`：TOTP 密钥生成、HMAC 计算、时间窗口验证
- `EmailService`：SMTP 邮件发送（libcurl 传输层）

### Validation（`src/validation/` + `include/fulla/drogon/validation/`）
- `Rule`、`RuleSet`、`RuleEngine`、`HttpResponder`：请求体验证规则引擎（从 `oauth2::validation` 迁移，旧命名空间保留兼容 shim）

### Error（`src/error/` + `include/fulla/drogon/error/`）
- `ErrorHandler`：Drogon 全局异常处理器
- `ErrorResponder`：构建标准 Error Envelope 响应
- `OAuth2ErrorHandler`：OAuth2 特定错误映射（RFC 6749 §5.2）
- `RequestId`：从请求头/生成请求 ID

### Observability（`src/observability/` + `include/fulla/drogon/observability/`）
- `AuditLogger`：结构化审计日志（JSON 格式，写入 `logs/audit.json`）
- `Metrics`：Prometheus 指标（请求计数、延迟、错误率）
- `OpenApiGenerator`：从 Controller 注解生成 OpenAPI 3.0 spec

## 开发约束

### 1. `[this]` 捕获规则
- **Controllers**：`[this]` **允许且普遍**。Drogon `HttpController<T,false>` 是进程级单例，由 Drogon 用裸指针管理，存活整个进程。`shared_from_this()` 不适用（不继承 `enable_shared_from_this`）。
- **Plugin/Services**：**禁止** `[this]` 和 `[&var]`，必须 `auto self = shared_from_this()` 后捕获 `self`（见 `.claude/rules/db-operations.md`）。

### 2. Controller 注册
- `AutoCreation=false` 的 Controller 必须在 `bootstrap::registerAllControllers()` 中手动注册（见 `apps/server/src/bootstrap/ControllerRegistration.cc`）。
- 每个 Controller 的 `initApiDocs()` 必须在 `main.cc` 中显式调用（避免循环依赖，`OAuth2Plugin` 不再自动调用）。

### 3. Plugin 单例访问
- `OAuth2Plugin` 是 Drogon Plugin 单例，通过 `drogon::app().getPlugin<OAuth2Plugin>()` 访问。
- **性能关键路径**：Controller/Filter 应缓存 Plugin 指针（通过 `setPlugin()` 注入），避免每次请求查找。`main.cc` 的 `registerBeginningAdvice` 在启动时完成注入。

### 4. 依赖方向
- ✅ `libs/drogon` → `libs/common`、`libs/oauth2`、`libs/identity`、`libs/storage-*`
- ❌ `libs/common`、`libs/oauth2`、`libs/identity` → `libs/drogon`（Domain 层禁止 Drogon 依赖）

### 5. 条件编译
- `WITH_SOCIAL`：启用社交登录 Controller（Google/WeChat/GitHub）
- `WITH_WEBAUTHN`：启用 WebAuthn/FIDO2 Controller
- 禁用时，对应 `.cc` 文件从编译中排除，`#ifdef WITH_*` 守卫确保头文件一致性

### 6. DB 访问
- **方向**：新增 Controller 内的 DB 读取走 Repository（`libs/storage-postgres`，如
  `ClientOwnersRepository`——v1.5.0 M0 起 AEC consent 属主查询即经它）或 Domain 服务
  （`libs/oauth2`、`libs/identity`）；触碰既有直查代码时顺带迁移（migrate-on-touch）。
- **登记例外（#222 写实，v1.5.0 M0）**：本目录约 18 个文件存在直接 `Mapper<...>(db)`
  历史存量（AuthService、admin/* 等），未设上限执法、非本里程碑清偿范围——旧规则
  「一律禁止」与现实脱节，故校准为上述方向性约定；不新增例外文件。
- 所有 DB 操作遵循 `.claude/rules/db-operations.md`：async callback + Mapper + Criteria 三件套。

### 7. Error 处理
- 所有错误响应必须通过 `ErrorResponder::buildResponse()` 构建标准 Error Envelope。
- OAuth2 错误码映射到 RFC 6749 §5.2（`invalid_request`、`invalid_client`、`invalid_grant`、`unauthorized_client`、`unsupported_grant_type`）。

### 8. 测试
- Adapter 层单元测试在 `libs/drogon/test/`，链接 Drogon 但不连 DB。
- 集成测试在 `tests/integration/`，使用 `tests/test_main.cc` 的 Drogon Test 框架。

## 相关规则

- 根 `AGENTS.md`：全局规则索引
- `.claude/rules/db-operations.md`：DB 操作规则（async callback + Mapper + Criteria）
- `.claude/rules/data-access.md`：存储层触发器
- `.claude/rules/orm-models.md`：ORM 模型禁止手改
- `.claude/rules/dev-workflow.md`：构建/测试命令（`./manage.sh`、`./manage.ps1`）
