# OAuth2 API 接口文档

> **完整 API 规范**: 手工维护的 OpenAPI 源文件为 [`apps/server/openapi.yaml`](https://github.com/voidvec/fulla/blob/master/apps/server/openapi.yaml)（**唯一契约源**，CI 通过 `openapi-spec-validator` + 治理门校验三层一致性与版本同步）；Swagger UI（`/docs/api`）在线浏览的是运行时由 Controller 代码生成的 `apps/server/docs/api/openapi.json`（派生产物）。

本服务提供基于 OAuth2.0 标准（RFC 6749）的认证授权服务。

## 端点分类概览 (Endpoint Categories)

| 分类 | 描述 | 前缀 |
|------|------|------|
| **Password Reset** | 密码重置请求与确认（基于邮件验证码） | `/api/password-reset` |
| **Email Verification** | 邮箱验证发送与确认 | `/api/email/verify` |
| **MFA (Multi-Factor Auth)** | TOTP 设置、验证、恢复码管理 | `/api/me/mfa`（登录补全为 `/oauth2/mfa/verify`） |
| **Admin API** | 用户管理、客户端管理、审计日志（需 admin 角色） | `/api/admin` |
| **User Self-Service** | 用户个人资料更新、密码修改、会话管理 | `/api/user` |
| **OIDC Discovery** | OpenID Connect 发现端点与 JWKS | `/.well-known/openid-configuration`, `/oauth2/jwks` |

---

## 1. 授权端点 (Authorization Endpoint)

用于请求用户授权，获取 Authorization Code。

- **URL**: `/oauth2/authorize`
- **Method**: `GET`
- **Access**: 公开 (需登录)

### 请求参数 (Query Parameters)

| 参数名 | 必选 | 描述 | 示例 |
|---|---|---|---|
| `response_type` | 是 | 必须为 `code` | `code` |
| `client_id` | 是 | 客户端 ID | `fulla-portal` |
| `redirect_uri` | 是 | 回调地址 (需完全匹配) | `http://localhost:5173/callback` |
| `scope` | 否 | 申请的权限范围 | `openid profile` |
| `state` | 建议 | 防止 CSRF 的随机串 | `xyz123` |
| `code_challenge` | 否 | PKCE code challenge（PUBLIC 客户端默认强制） | `dBjftJeZ4CVK...` |
| `code_challenge_method` | 否 | `plain` 或 `S256`（提供 challenge 时默认 `plain`） | `S256` |
| `nonce` | 否 | OIDC nonce（防重放），openid scope 时回显到 id_token | `n-0S6_WzA2Mj` |
| `prompt` | 否 | OIDC 提示值，空格分隔：`none`/`login`/`consent`/`select_account`（§3.1.2.1）。`none` 禁止 UI；`login` 强制重认证；`consent` 强制同意页。`none` 与其他值并用 → 400 | `none` |
| `max_age` | 否 | 认证最大允許年齡（秒）。session auth_time 超齡 → 強制重认证 | `3600` |

### 响应

**成功响应**：
重定向至 `redirect_uri`，并附带 `code` 和 `state`。

```http
HTTP/1.1 302 Found
Location: http://localhost:5173/callback?code=SplxlOBeZQQYbYS6WxSbIA&state=xyz123
```

**错误响应**：
直接返回 JSON 错误或重定向带 error 参数。

```json
{
  "error": "invalid_client",
  "error_description": "Unknown client_id"
}
```

---

## 2. 令牌端点 (Token Endpoint)

用于使用 Authorization Code 换取 Access Token。

- **URL**: `/oauth2/token`
- **Method**: `POST`
- **Access**: 公开 (需 Client 认证)
- **Content-Type**: `application/x-www-form-urlencoded`

### 请求参数 (Form Data)

| 参数名 | 必选 | 描述 | 示例 |
|---|---|---|---|
| `grant_type` | 是 | 必须为 `authorization_code` | `authorization_code` |
| `code` | 是 | 上一步获取的 code | `SplxlOBeZQQYbYS6WxSbIA` |
| `redirect_uri` | 是 | 必须与获取 code 时一致 | `http://localhost:5173/callback` |
| `client_id` | 是 | 客户端 ID | `fulla-portal` |
| `client_secret` | 是 | 客户端密钥 (用于验证) | `vue-secret` |

### 响应

**成功 (200 OK)**:

```json
{
  "access_token": "2YotnFZFEjr1zCsicMWpAA",
  "token_type": "Bearer",
  "expires_in": 3600,
  "refresh_token": "tGzv3JOkF0XG5Qx2TlKWIA",
  "scope": "openid profile"
}
```

**响应头（F-019，RFC 6749 §5.1 / RFC 7009 §2.2.1）**：所有 token / introspect /
revoke 成功响应都带 `Cache-Control: no-store` 与 `Pragma: no-cache`，禁止中间
代理缓存含憑证的响应体。

*(注：`grant_type=refresh_token` 需先通过客户端认证（F-003/F-017，RFC 6749 §3.2.1/§6）：认证方式必须匹配客户端注册的 `token_endpoint_auth_method`——`client_secret_basic`（默认）仅接受 HTTP Basic（body 携带 `client_secret` 会被拒）；`client_secret_post` 仅接受表单字段；PUBLIC 客户端仅发 `client_id`（携带 secret 会被拒）。缺失或错误返回 401 `invalid_client`。refresh token 持久化仅 Postgres 后端支持；`storage_type="redis"` 已弃用，该模式下 refresh grant 返回 `unsupported_grant_type`（F-005）。)*

**失败 (400/401)**:

```json
{
  "error": "invalid_grant",
  "error_description": "Authorization code has expired"
}
```

**失败 (429 Too Many Requests)** — F-018 限流：`/oauth2/token`、
`/oauth2/introspect`、`/oauth2/revoke` 与 device_code 轮询共享一個进程内滑动窗口
限流器，按 `(client_ip, client_id)` 分桶。窗口内（默认 60s）**失败**計数達閾值
（默认 30，可經 `custom_config["auth"]["rate_limit"]` 配置 `max_failures` /
`window_seconds`）后，后续請求返回 429。仅計失败（认证/校验失败），成功清零。

```http
HTTP/1.1 429 Too Many Requests
Retry-After: 42
Content-Type: application/json

{
  "error": "invalid_request",
  "error_description": "Too many failed attempts; please retry later"
}
```

---

## 3. 用户信息端点 (UserInfo Endpoint)

用于验证 Access Token 并获取用户信息。

- **URL**: `/oauth2/userinfo`
- **Method**: `GET`
- **Access**: 受保护 (Bearer Token)

### 请求头 (Headers)

Authorization: `Bearer {access_token}`

### 响应

**成功 (200 OK)**:

```json
{
  "sub": "admin",
  "name": "admin",
  "email": "admin@example.com",
  "email_verified": true,
  "picture": "..."
}
```

**失败 (401 Unauthorized)**:

```json
{
  "error": "invalid_token"
}
```

**失败 (403 Forbidden)** — F-023：access token scope 不含 `openid`，或为 M2M
token（subject `client:*`）。响应附带
`WWW-Authenticate: Bearer error="insufficient_scope"`：

```json
{
  "error": "insufficient_scope",
  "error_description": "The access token does not have the openid scope required for userinfo"
}
```

### 3.x 路徑→required-scope 映射（F-010 最小资源-scope 模型）

`OAuth2AuthFilter` / `AuthorizationFilter` 在 access token 校验通过后，按請求
路徑強制最小 required-scope（RFC 6750 §3.1）。token scope 不足时返回 403，
响应附带 `WWW-Authenticate: Bearer realm="fulla", error="insufficient_scope",
scope="<required>"`，其中 `scope` 屬性命名解锁该资源所需的 scope。

| 路徑 | Required Scope | 備注 |
|---|---|---|
| `/oauth2/userinfo` | `openid` | 与 userinfo handler 内的 F-023 检查并存（defense-in-depth） |
| `/api/me`、`/api/me/*` | `profile` | 經 `OAuth2AuthFilter` |
| `/api/admin/*` | `admin` | 經 `AuthorizationFilter`，**疊加在既有 RBAC 角色检查之上**（scope 閘門先跑，角色閘門后跑，两者都須通过） |

> **完整资源-scope 授權模型为后续工作**（独立 issue「完整资源-scope 授權模型」）。
> 當前仅上述最小映射；其餘 `/api/*` 路徑仍仅由既有 RBAC 规則（`rbac_rules`）
> 把关，不额外要求特定 scope。Scope 匹配为空格分隔 token 的精确匹配
> （`fulla::drogon::utils::hasScope()`），避免 `openidprofile` 误过
> `openid`/`profile`。

### 3.y 客户端管理（F-030：admin-only，无 RFC 7592 自管理）

客户端注册与管理**仅**經 admin API `/api/admin/clients/*`（需 admin scope +
admin 角色）。本服務**不**实作 RFC 7592 動態客户端管理的
`registration_access_token` 自管理端点 —— 客户端无法自助查看/修改自身注册
信息。需要變更的客户端須联繫管理員經 admin API 处理。

### 3.z nonce 重放防護（F-026：客户端責任）

OIDC Core §15.5.2 规定 nonce 重放检查为**客户端 MUST**：服務端在 id_token 中
**回顯**（echo）客户端提交的 nonce，但**不**为其存储或做服務端重放检查。客户端
必須（1）为每次认证請求生成唯一的 nonce，（2）在收到 id_token 后比对回顯值与
本地 nonce，并（3）拒絕重複或缺失 nonce 的 id_token。本服務遵循此分工，不
提供服務端 nonce 重放防護。

---

## 3.1 RP-Initiated Logout 端点 (End Session Endpoint)

OIDC RP-Initiated Logout 1.0 §2 — 終止用户的 server-side session，并（可選）重
定向到客户端注册的 `post_logout_redirect_uri`。

- **URL**: `/oauth2/end_session`
- **Method**: `GET`（鏈接式）或 `POST`（表單式）
- **Access**: 公开（不需 Bearer token）

### 請求參数 (Query/Form)

| 參数名 | 必選 | 描述 |
|---|---|---|
| `id_token_hint` | 否* | 此前签发的 id_token，其 `aud` 声明标识客户端用于校验 `post_logout_redirect_uri`（签名强制校验：RS256 + kid 匹配 + iss/exp/sub 策略；验签失败返回 400 AUTH_INVALID_ID_TOKEN_HINT，错误码 4006）。*提供 `post_logout_redirect_uri` 时必需 |
| `post_logout_redirect_uri` | 否 | 登出后重定向 URI，須为 `id_token_hint` 客户端注册的 redirect_uri，否則 400 |
| `state` | 否 | 不透明值，原样回顯到重定向 URI |

### 响应

- **200 OK**：未提供 `post_logout_redirect_uri` 时，返回 `{ "message": "Logged out successfully" }`，session 已清除。
- **302 Found**：提供并校验通过的 `post_logout_redirect_uri`（附 `state`）。
- **400 Bad Request**：`post_logout_redirect_uri` 未注册 / 缺 `id_token_hint` 无法标识客户端 / `id_token_hint` 验签失败（过期、issuer 不符、签名无效，错误码 4006）。

---

## 4. 辅助接口 (Helper Endpoints)

### 登录提交 (Internal)

- **URL**: `/oauth2/login`
- **Method**: `POST`
- **Desc**: 内部使用的表单提交接口，用于 Session 登录并重定向。

### WeChat 登录 (Optional)

- **URL**: `/api/wechat/login`
- **Method**: `POST`
- **Desc**: 处理微信小程序/扫码登录（演示用途）。

### Google 登录回调 (Optional)

- **URL**: `/api/google/login`
- **Method**: `POST`
- **Desc**: 接收前端传来的 Google Authorization Code，服务端向 Google 换取 Access Token 并调用 UserInfo API，返回过滤后的用户信息（`sub`, `name`, `email`, `picture`）。
- **请求参数**:
  - `code` (required): Google 返回的授权码
- **成功 (200 OK)**:
  ```json
  {"sub": "1234567890", "name": "John Doe", "email": "john@gmail.com", "picture": "..."}
  ```
- **失败 (400/502)**: code 无效或 Google API 不可达。

### 用户注册

- **URL**: `/api/register`
- **Method**: `POST`
- **Content-Type**: `application/x-www-form-urlencoded`
- **限流**: 每IP每分钟最多 5 次，全局每分钟 5000 次（Hodor 插件）

#### 请求参数 (Form Data)

| 参数名 | 必选 | 描述 |
|---|---|---|
| `username` | 是 | 用户名 |
| `password` | 是 | 密码（明文，服务端 SHA256+Salt 存储）|
| `email` | 否 | 邮件地址 |

#### 响应

- **成功 (200 OK)**: `User Registered`
- **失败 (400 Bad Request)**: 缺少用户名或密码
- **失败 (500 Internal Server Error)**: 用户名已存在等

### 管理员 Dashboard (RBAC Protected)

- **URL**: `/api/admin/dashboard`
- **Method**: `GET`
- **Access**: 受保护，需 `admin` 角色（Header: `Authorization: Bearer <token>`）

#### 响应

- **成功 (200 OK)**:
  ```json
  {"message": "Welcome to Admin Dashboard", "status": "success"}
  ```
- **失败 (401)**: Token 无效或缺失
- **失败 (403)**: 用户已登录但不具备 `admin` 角色

---

## 5. 通用错误码

> **单一权威来源（single source of truth）**：本章节 5.1 与 5.2 的表格由后端 `ErrorCatalog`（`libs/common/include/fulla/common/error/ErrorCatalog.h`）的 `allEntries()` / `allOAuthEntries()` 生成并由自动化测试校验，请勿手工修改表格行。
> 任一不一致（缺失/多余条目、HTTP 状态码或 Error_Category 不匹配）都会导致校验测试失败：`fulla-tests -r ErrorCatalogDoc`。

### 5.1 应用错误码 (Application Error Codes)

业务端点（Application_Endpoint）返回统一的 Error Envelope，其 `error.code` 取值属于下表登记的 Error_Code 集合；`numeric_code` 与 `category` 同样取自下表，HTTP 状态码按 Error_Category（NETWORK 类按 numeric_code 区分 502/504）一致映射。少数面向资源语义的 VALIDATION 码通过条目级显式覆盖保留迁移前的 HTTP 状态码（方案 A / 需求 11.4）：`VALIDATION_RESOURCE_NOT_FOUND` → 404，资源已存在/冲突类（`VALIDATION_RESOURCE_CONFLICT`、`VALIDATION_USERNAME_TAKEN`、`VALIDATION_EMAIL_TAKEN`、`VALIDATION_CREDENTIAL_ALREADY_REGISTERED`）→ 409，`VALIDATION_RATE_LIMITED` → 429；其余 VALIDATION 码仍为 400。

| Error_Code | numeric_code | Error_Category | HTTP Status | 默认信息 (Client_Safe_Message) |
|---|---|---|---|---|
| `NET_CONNECTION_FAILED` | 1001 | NETWORK | 502 | 上游连接失败 |
| `NET_TIMEOUT` | 1002 | NETWORK | 504 | 请求超时 |
| `DB_CONNECTION_ERROR` | 2001 | DATABASE | 500 | 服务暂时不可用 |
| `DB_QUERY_ERROR` | 2002 | DATABASE | 500 | 服务暂时不可用 |
| `DB_CONSTRAINT_VIOLATION` | 2003 | DATABASE | 500 | 数据冲突 |
| `VALIDATION_INVALID_INPUT` | 3001 | VALIDATION | 400 | 输入参数有误 |
| `VALIDATION_MISSING_REQUIRED_FIELD` | 3002 | VALIDATION | 400 | 缺少必填字段 |
| `VALIDATION_FORMAT_ERROR` | 3003 | VALIDATION | 400 | 格式不正确 |
| `VALIDATION_PASSWORD_TOO_SHORT` | 3014 | VALIDATION | 400 | 密码长度不足 |
| `VALIDATION_RESOURCE_NOT_FOUND` | 3004 | VALIDATION | 404 | 资源不存在 |
| `VALIDATION_RESOURCE_CONFLICT` | 3005 | VALIDATION | 409 | 资源已存在或冲突 |
| `VALIDATION_USERNAME_TAKEN` | 3006 | VALIDATION | 409 | 该用户名已被注册 |
| `VALIDATION_EMAIL_TAKEN` | 3007 | VALIDATION | 409 | 该邮箱已被注册 |
| `VALIDATION_CREDENTIAL_ALREADY_REGISTERED` | 3008 | VALIDATION | 409 | 该安全密钥已注册，无需重复添加 |
| `VALIDATION_RESET_TOKEN_INVALID` | 3009 | VALIDATION | 400 | 重置链接已失效，请重新申请 |
| `VALIDATION_VERIFICATION_TOKEN_INVALID` | 3010 | VALIDATION | 400 | 验证链接已失效，请重新发送邮件 |
| `VALIDATION_DEVICE_CODE_INVALID` | 3011 | VALIDATION | 400 | 设备码无效、已过期或已被处理 |
| `VALIDATION_RATE_LIMITED` | 3012 | VALIDATION | 429 | 请求过于频繁，请稍后重试 |
| `AUTH_INVALID_CREDENTIALS` | 4001 | AUTHENTICATION | 401 | 用户名或密码错误 |
| `AUTH_TOKEN_EXPIRED` | 4002 | AUTHENTICATION | 401 | 登录已过期 |
| `AUTH_TOKEN_INVALID` | 4003 | AUTHENTICATION | 401 | 登录凭证无效 |
| `AUTH_SESSION_REQUIRED` | 4007 | AUTHENTICATION | 401 | 需要先登录 |
| `AUTH_MFA_CODE_INVALID` | 4004 | AUTHENTICATION | 401 | 验证码不正确 |
| `AUTH_MFA_NOT_CONFIGURED` | 4005 | AUTHENTICATION | 401 | 尚未设置双重验证，请先完成设置 |
| `AUTH_INVALID_ID_TOKEN_HINT` | 4006 | AUTHENTICATION | 400 | 登录令牌提示无效 |
| `AUTH_MFA_REQUIRED` | 4008 | AUTHENTICATION | 401 | 需要完成 MFA 验证 |
| `AUTH_PASSWORD_CHANGE_REQUIRED` | 4009 | AUTHENTICATION | 403 | 必须先修改密码 |
| `AUTHZ_ACCESS_DENIED` | 5001 | AUTHORIZATION | 403 | 没有访问权限 |
| `AUTHZ_INSUFFICIENT_PERMISSIONS` | 5002 | AUTHORIZATION | 403 | 权限不足 |
| `INTERNAL_ERROR` | 6001 | INTERNAL | 500 | 服务器内部错误 |

### 5.2 OAuth2 协议错误码 (RFC 6749 §5.2 / RFC 7009 / RFC 8628)

OAuth2 协议端点（OAuth2_Protocol_Endpoint）保持 RFC 6749 §5.2 错误体结构 `{ "error", "error_description", "error_uri" }`，其 `error` 取值与 HTTP 状态码取自下表。

| error | HTTP Status | 默认 error_description |
|---|---|---|
| `invalid_request` | 400 | 请求参数缺失或无效 |
| `invalid_client` | 401 | 客户端认证失败 |
| `invalid_grant` | 400 | 授权许可无效或已过期 |
| `unauthorized_client` | 400 | 客户端无权使用该授权类型 |
| `unsupported_grant_type` | 400 | 不支持的授权类型 |
| `invalid_scope` | 400 | 请求的 scope 无效 |
| `server_error` | 500 | 服务器内部错误 |
| `temporarily_unavailable` | 503 | 服务暂时不可用 |
| `access_denied` | 403 | 授权请求被拒绝（用户无权或拒绝授权） |
| `unsupported_token_type` | 400 | 不支持的令牌类型 |
| `authorization_pending` | 400 | 授权尚未完成，请稍后重试 |
| `slow_down` | 400 | 轮询过于频繁，请降低频率 |
| `expired_token` | 400 | 设备码已过期，请重新发起授权 |

### 5.3 HTTP 状态码速查

| HTTP Status | 描述 | 原因示例 |
|---|---|---|
| `200` | OK | 请求成功 |
| `302` | Found | 重定向 (如 OAuth2 授权跳转) |
| `400` | Bad Request | 参数错误, `invalid_grant`, `unauthorized_client` |
| `401` | Unauthorized | Token 无效或过期, `invalid_client` |
| `403` | Forbidden | **RBAC 拦截**: 用户已登录但缺少所需角色, `access_denied` |
| `429` | Too Many Requests | 触发限流 (Rate Limiting) |
| `500` | Internal Server Error | 服务器内部错误 |

---

## 6. API 契约维护流程（OpenAPI 治理）

HTTP API 契约的**单一事实源是 [`apps/server/openapi.yaml`](https://github.com/voidvec/fulla/blob/master/apps/server/openapi.yaml)**；本篇是它的导读与补全（含错误码表等 yaml 未承载的内容）。

### 6.1 变更流程

1. 修改端点行为时，同步修改 `apps/server/openapi.yaml`（新端点 / 参数 / 响应）。
2. Controller 内经 `OpenApiGenerator::addEndpoint()` 登记的元数据保持一致（`fulla-tests -r OpenApiGenerator` 校验登记完整性）。
3. Swagger UI（`http://localhost:5555/docs/api/`）由服务器托管，用于人工核对。

### 6.2 破坏性变更门（CI）

`OpenAPI Governance` workflow 在 PR 上运行 **oasdiff breaking** 门：对比 PR 与 master 的 `openapi.yaml`，任何破坏性变更（删路径、收紧请求体、收窄响应等）都会使 CI 失败，除非：

- 变更伴随主版本号升级；或
- 在 `tools/openapi-governance/oasdiff-breaking-ignore.md` 中显式豁免并写明理由。

本地可复现同一命令（见 `.github/workflows/openapi-governance.yml` 头部注释）。

### 6.3 质量标准

*   **必需字段**：`path`, `method`, `summary`, `description`, `tags`, `responses`, `requiresAuth`。
*   **推荐做法**：为每个响应码提供 `responseExamples`，并详细定义参数的 `type` 和 `location`。

### 6.4 故障排查

*   **Swagger UI 无法访问**：检查静态资源是否随服务器分发，确认静态文件服务已启用。
*   **登记校验失败**：运行 `fulla-tests -r OpenApiGenerator` 单元测试，查看具体的注册错误。
*   **治理门误报**：核对 `oasdiff breaking` 输出与豁免清单条目是否对应同一 path/operation。

