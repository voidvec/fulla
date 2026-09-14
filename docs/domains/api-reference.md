# OAuth2 API Reference

> **Complete API specification**: The hand-maintained OpenAPI source file is [`apps/server/openapi.yaml`](https://github.com/voidvec/fulla/blob/master/apps/server/openapi.yaml) (the **single source of truth**; CI validates three-layer consistency and version synchronization via `openapi-spec-validator` plus a governance gate). The Swagger UI (`/docs/api`) browses `apps/server/docs/api/openapi.json`, a derived artifact generated at runtime from Controller code.

This service provides authentication and authorization based on the OAuth 2.0 standard (RFC 6749).

## Endpoint Category Overview

| Category | Description | Prefix |
|------|------|------|
| **Password Reset** | Password reset requests and confirmation (based on email verification codes) | `/api/password-reset` |
| **Email Verification** | Email verification sending and confirmation | `/api/email/verify` |
| **MFA (Multi-Factor Auth)** | TOTP setup, verification, and recovery code management | `/api/me/mfa` (login completion at `/oauth2/mfa/verify`) |
| **Admin API** | User management, client management, audit logs (requires the admin role) | `/api/admin` |
| **User Self-Service** | User profile updates, password changes, session management | `/api/user` |
| **OIDC Discovery** | OpenID Connect discovery endpoints and JWKS | `/.well-known/openid-configuration`, `/oauth2/jwks` |

---

## 1. Authorization Endpoint

Used to request user authorization and obtain an authorization code.

- **URL**: `/oauth2/authorize`
- **Method**: `GET`
- **Access**: Public (requires login)

### Request Parameters (Query Parameters)

| Parameter | Required | Description | Example |
|---|---|---|---|
| `response_type` | Yes | Must be `code` | `code` |
| `client_id` | Yes | Client ID | `fulla-portal` |
| `redirect_uri` | Yes | Callback URL (must match exactly) | `http://localhost:5173/callback` |
| `scope` | No | Requested scope | `openid profile` |
| `state` | Recommended | Random string for CSRF protection | `xyz123` |
| `code_challenge` | No | PKCE code challenge (mandatory by default for PUBLIC clients) | `dBjftJeZ4CVK...` |
| `code_challenge_method` | No | `plain` or `S256` (defaults to `plain` when a challenge is provided) | `S256` |
| `nonce` | No | OIDC nonce (replay protection); echoed into the id_token when the openid scope is requested | `n-0S6_WzA2Mj` |
| `prompt` | No | OIDC prompt values, space-separated: `none`/`login`/`consent`/`select_account` (§3.1.2.1). `none` forbids any UI; `login` forces re-authentication; `consent` forces the consent page. Combining `none` with other values → 400 | `none` |
| `max_age` | No | Maximum allowable age of authentication (seconds). If the session auth_time exceeds the limit → forced re-authentication | `3600` |

### Response

**Success**:
Redirects to `redirect_uri` with `code` and `state` attached.

```http
HTTP/1.1 302 Found
Location: http://localhost:5173/callback?code=SplxlOBeZQQYbYS6WxSbIA&state=xyz123
```

**Error**:
Returns a JSON error directly, or redirects with an error parameter.

```json
{
  "error": "invalid_client",
  "error_description": "Unknown client_id"
}
```

---

## 2. Token Endpoint

Used to exchange an authorization code for an access token.

- **URL**: `/oauth2/token`
- **Method**: `POST`
- **Access**: Public (requires client authentication)
- **Content-Type**: `application/x-www-form-urlencoded`

### Request Parameters (Form Data)

| Parameter | Required | Description | Example |
|---|---|---|---|
| `grant_type` | Yes | Must be `authorization_code` | `authorization_code` |
| `code` | Yes | The code obtained in the previous step | `SplxlOBeZQQYbYS6WxSbIA` |
| `redirect_uri` | Yes | Must be identical to the one used to obtain the code | `http://localhost:5173/callback` |
| `client_id` | Yes | Client ID | `fulla-portal` |
| `client_secret` | Yes | Client secret (used for authentication) | `vue-secret` |

### Response

**Success (200 OK)**:

```json
{
  "access_token": "2YotnFZFEjr1zCsicMWpAA",
  "token_type": "Bearer",
  "expires_in": 3600,
  "refresh_token": "tGzv3JOkF0XG5Qx2TlKWIA",
  "scope": "openid profile"
}
```

**Response headers (F-019, RFC 6749 §5.1 / RFC 7009 §2.2.1)**: All successful token /
introspect / revoke responses carry `Cache-Control: no-store` and `Pragma: no-cache`,
forbidding intermediary proxies from caching response bodies that contain credentials.

*(Note: `grant_type=refresh_token` requires prior client authentication (F-003/F-017, RFC 6749 §3.2.1/§6): the authentication method must match the client's registered `token_endpoint_auth_method` — `client_secret_basic` (default) accepts only HTTP Basic (a `client_secret` in the body is rejected); `client_secret_post` accepts only form fields; PUBLIC clients send only `client_id` (including a secret is rejected). Missing or incorrect credentials return 401 `invalid_client`. Refresh token persistence is supported only on the Postgres backend; `storage_type="redis"` is deprecated, and in that mode the refresh grant returns `unsupported_grant_type` (F-005).)*

**Failure (400/401)**:

```json
{
  "error": "invalid_grant",
  "error_description": "Authorization code has expired"
}
```

**Failure (429 Too Many Requests)** — F-018 rate limiting: `/oauth2/token`,
`/oauth2/introspect`, `/oauth2/revoke`, and device_code polling share a single
in-process sliding-window rate limiter, bucketed by `(client_ip, client_id)`. Within
a window (default 60 s), once **failed** attempts reach the threshold (default 30;
configurable via `custom_config["auth"]["rate_limit"]` with `max_failures` /
`window_seconds`), subsequent requests return 429. Only failures (authentication /
validation failures) are counted; a success resets the counter.

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

## 3. UserInfo Endpoint

Used to validate an access token and retrieve user information.

- **URL**: `/oauth2/userinfo`
- **Method**: `GET`
- **Access**: Protected (Bearer token)

### Request Headers

Authorization: `Bearer {access_token}`

### Response

**Success (200 OK)**:

```json
{
  "sub": "admin",
  "name": "admin",
  "email": "admin@example.com",
  "email_verified": true,
  "picture": "..."
}
```

**Failure (401 Unauthorized)**:

```json
{
  "error": "invalid_token"
}
```

**Failure (403 Forbidden)** — F-023: the access token's scope does not include
`openid`, or the token is an M2M token (subject `client:*`). The response carries
`WWW-Authenticate: Bearer error="insufficient_scope"`:

```json
{
  "error": "insufficient_scope",
  "error_description": "The access token does not have the openid scope required for userinfo"
}
```

### 3.x Path → required-scope mapping (F-010 minimal resource-scope model)

After an access token passes validation, `OAuth2AuthFilter` / `AuthorizationFilter`
enforce a minimal required scope based on the request path (RFC 6750 §3.1). When the
token's scope is insufficient, a 403 is returned with `WWW-Authenticate: Bearer
realm="fulla", error="insufficient_scope", scope="<required>"`, where the `scope`
attribute names the scope required to unlock the resource.

| Path | Required Scope | Notes |
|---|---|---|
| `/oauth2/userinfo` | `openid` | Coexists with the F-023 check inside the userinfo handler (defense-in-depth) |
| `/api/me`, `/api/me/*` | `profile` | Enforced via `OAuth2AuthFilter` |
| `/api/admin/*` | `admin` | Enforced via `AuthorizationFilter`, **layered on top of the existing RBAC role check** (the scope gate runs first, then the role gate; both must pass) |

> **A complete resource-scope authorization model is future work** (separate issue
> "Complete resource-scope authorization model"). Only the minimal mapping above
> applies today; all other `/api/*` paths remain guarded solely by the existing RBAC
> rules (`rbac_rules`), with no additional scope requirement. Scope matching is exact
> matching against space-separated tokens (`fulla::drogon::utils::hasScope()`),
> preventing `openidprofile` from erroneously passing `openid`/`profile`.

### 3.y Client management (F-030: admin-only, no RFC 7592 self-management)

Client registration and management are available **only** via the admin API
`/api/admin/clients/*` (requires the admin scope + admin role). This service does
**not** implement the `registration_access_token` self-management endpoints of
RFC 7592 dynamic client management — clients cannot view or modify their own
registration information. Clients that require changes must contact an
administrator to process them via the admin API.

### 3.z Nonce replay protection (F-026: client responsibility)

OIDC Core §15.5.2 makes nonce replay checking a **client-side MUST**: the server
**echoes** the client-submitted nonce in the id_token but does not store it or
perform server-side replay checks. Clients must (1) generate a unique nonce for
every authentication request, (2) after receiving an id_token, compare the echoed
value against the locally stored nonce, and (3) reject any id_token with a
duplicated or missing nonce. This service follows that division of responsibility
and provides no server-side nonce replay protection.

---

## 3.1 RP-Initiated Logout Endpoint (End Session Endpoint)

OIDC RP-Initiated Logout 1.0 §2 — terminates the user's server-side session and
(optionally) redirects to the client's registered `post_logout_redirect_uri`.

- **URL**: `/oauth2/end_session`
- **Method**: `GET` (link-style) or `POST` (form-style)
- **Access**: Public (no Bearer token required)

### Request Parameters (Query/Form)

| Parameter | Required | Description |
|---|---|---|
| `id_token_hint` | No* | A previously issued id_token whose `aud` claim identifies the client, used to validate `post_logout_redirect_uri` (signature verification is mandatory: RS256 + kid match + iss/exp/sub policy; `aud` supports string or array per RFC 7519 §4.1.3, server tries each candidate). Verification failure returns 400 `AUTH_INVALID_ID_TOKEN_HINT` (error code 4006); unregistered `post_logout_redirect_uri` returns 400 `VALIDATION_REDIRECT_URI_NOT_REGISTERED` (error code 3013). *Required when `post_logout_redirect_uri` is provided |
| `post_logout_redirect_uri` | No | Post-logout redirect URI; must be a redirect_uri registered by the `id_token_hint` client, otherwise 400 |
| `state` | No | Opaque value echoed verbatim into the redirect URI |

### Response

- **200 OK**: When no `post_logout_redirect_uri` is provided, returns `{ "message": "Logged out successfully" }`; the session has been cleared.
- **302 Found**: A provided and successfully validated `post_logout_redirect_uri` (with `state` attached).
- **400 Bad Request**: `post_logout_redirect_uri` not registered (`VALIDATION_REDIRECT_URI_NOT_REGISTERED`, error code 3013) / missing `id_token_hint` so the client cannot be identified (`AUTH_INVALID_ID_TOKEN_HINT`, 4006) / `id_token_hint` signature verification failed (expired, issuer mismatch, invalid signature; `AUTH_INVALID_ID_TOKEN_HINT`, 4006).

---

## 4. Helper Endpoints

### Login Submission (Internal)

- **URL**: `/oauth2/login`
- **Method**: `POST`
- **Desc**: An internal form-submission endpoint used for session login and redirect.

### WeChat Login (Optional)

- **URL**: `/api/wechat/login`
- **Method**: `POST`
- **Desc**: Handles WeChat Mini Program / QR-code login (for demonstration purposes).

### Google Login Callback (Optional)

- **URL**: `/api/google/login`
- **Method**: `POST`
- **Desc**: Receives the Google authorization code from the frontend, the server exchanges it with Google for an access token and calls the UserInfo API, returning filtered user information (`sub`, `name`, `email`, `picture`).
- **Request parameters**:
  - `code` (required): The authorization code returned by Google
- **Success (200 OK)**:
  ```json
  {"sub": "1234567890", "name": "John Doe", "email": "john@gmail.com", "picture": "..."}
  ```
- **Failure (400/502)**: Invalid code or the Google API is unreachable.

### User Registration

- **URL**: `/api/register`
- **Method**: `POST`
- **Content-Type**: `application/x-www-form-urlencoded`
- **Rate limit**: 5 requests per minute per IP, 5000 per minute globally (Hodor plugin)

#### Request Parameters (Form Data)

| Parameter | Required | Description |
|---|---|---|
| `username` | Yes | Username |
| `password` | Yes | Password (plaintext; stored server-side as SHA256 + salt) |
| `email` | No | Email address |

#### Response

- **Success (200 OK)**: `User Registered`
- **Failure (400 Bad Request)**: Missing username or password
- **Failure (500 Internal Server Error)**: Username already exists, etc.

### Admin Dashboard (RBAC Protected)

- **URL**: `/api/admin/dashboard`
- **Method**: `GET`
- **Access**: Protected; requires the `admin` role (Header: `Authorization: Bearer <token>`)

#### Response

- **Success (200 OK)**:
  ```json
  {"message": "Welcome to Admin Dashboard", "status": "success"}
  ```
- **Failure (401)**: Token invalid or missing
- **Failure (403)**: User is authenticated but does not hold the `admin` role

---

## 5. Common Error Codes

> **Single source of truth**: The tables in 5.1 and 5.2 below are generated from `allEntries()` / `allOAuthEntries()` of the backend `ErrorCatalog` (`libs/common/include/fulla/common/error/ErrorCatalog.h`) and verified by an automated test — do not modify table rows by hand.
> Any inconsistency (missing/extra entries, HTTP status code or Error_Category mismatch) fails the verification test: `fulla-tests -r ErrorCatalogDoc`.

### 5.1 Application Error Codes

> **Language note**: the `Default Message` / `Description` columns quote the **exact strings the server emits** (registered in `ErrorCatalog`); they are kept verbatim — currently Chinese — because a client matching on `error_description` must see precisely what the catalog defines. A CI test (`Unit_P0_ErrorCatalogDoc_*`) fails if these tables drift from the catalog.

Business endpoints (Application_Endpoint) return a uniform error envelope whose `error.code` values belong to the Error_Code set registered in the table below; `numeric_code` and `category` likewise come from the table, and the HTTP status code maps consistently by Error_Category (the NETWORK category distinguishes 502/504 by numeric_code). A few resource-semantics VALIDATION codes retain their pre-migration HTTP status codes via entry-level explicit overrides (Option A / requirement 11.4): `VALIDATION_RESOURCE_NOT_FOUND` → 404, resource-already-exists/conflict codes (`VALIDATION_RESOURCE_CONFLICT`, `VALIDATION_USERNAME_TAKEN`, `VALIDATION_EMAIL_TAKEN`, `VALIDATION_CREDENTIAL_ALREADY_REGISTERED`) → 409, `VALIDATION_RATE_LIMITED` → 429; all other VALIDATION codes remain 400.

| Error_Code | numeric_code | Error_Category | HTTP Status | Default Message (Client_Safe_Message) |
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

| `WEBAUTHN_INVALID_ATTESTATION` | 3015 | VALIDATION | 400 | 注册声明无法通过验证 |
| `WEBAUTHN_CHALLENGE_MISMATCH` | 3016 | VALIDATION | 400 | 注册挑战校验失败 |
| `VALIDATION_RESET_TOKEN_INVALID` | 3009 | VALIDATION | 400 | 重置链接已失效，请重新申请 |
| `VALIDATION_VERIFICATION_TOKEN_INVALID` | 3010 | VALIDATION | 400 | 验证链接已失效，请重新发送邮件 |
| `VALIDATION_DEVICE_CODE_INVALID` | 3011 | VALIDATION | 400 | 设备码无效、已过期或已被处理 |
| `VALIDATION_RATE_LIMITED` | 3012 | VALIDATION | 429 | 请求过于频繁，请稍后重试 |
| `VALIDATION_REDIRECT_URI_NOT_REGISTERED` | 3013 | VALIDATION | 400 | 登出重定向地址未注册 |
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
| `AUTH_SOCIAL_ACCOUNT_NOT_LINKED` | 5003 | AUTHORIZATION | 403 | 该第三方账号尚未绑定本地账户 |
| `INTERNAL_ERROR` | 6001 | INTERNAL | 500 | 服务器内部错误 |

### 5.2 OAuth2 Protocol Error Codes (RFC 6749 §5.2 / RFC 7009 / RFC 8628)

OAuth2 protocol endpoints (OAuth2_Protocol_Endpoint) keep the RFC 6749 §5.2 error body structure `{ "error", "error_description", "error_uri" }`; the `error` values and HTTP status codes are taken from the table below.

| error | HTTP Status | Default error_description |
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

### 5.3 HTTP Status Code Quick Reference

| HTTP Status | Description | Example Causes |
|---|---|---|
| `200` | OK | Request succeeded |
| `302` | Found | Redirect (e.g., the OAuth2 authorization jump) |
| `400` | Bad Request | Invalid parameters, `invalid_grant`, `unauthorized_client` |
| `401` | Unauthorized | Token invalid or expired, `invalid_client` |
| `403` | Forbidden | **RBAC block**: user is authenticated but lacks a required role, `access_denied` |
| `429` | Too Many Requests | Rate limit triggered (Rate Limiting) |
| `500` | Internal Server Error | Internal server error |

---

## 6. API Contract Maintenance Process (OpenAPI Governance)

The **single source of truth for the HTTP API contract is [`apps/server/openapi.yaml`](https://github.com/voidvec/fulla/blob/master/apps/server/openapi.yaml)**; this document is its guided introduction and complement (covering content the yaml does not carry, such as the error code tables).

### 6.1 Change Process

1. When changing endpoint behavior, update `apps/server/openapi.yaml` in sync (new endpoints / parameters / responses).
2. Metadata registered via `OpenApiGenerator::addEndpoint()` inside Controllers must stay consistent (`fulla-tests -r OpenApiGenerator` verifies registration completeness).
3. Swagger UI (`http://localhost:5555/docs/api/`) is hosted by the server for manual review.

### 6.2 Breaking-Change Gate (CI)

The `OpenAPI Governance` workflow runs an **oasdiff breaking** gate on PRs: it compares the PR's `openapi.yaml` against master, and any breaking change (removed paths, tightened request bodies, narrowed responses, etc.) fails CI unless:

- The change is accompanied by a major version bump; or
- It is explicitly exempted in `tools/openapi-governance/oasdiff-breaking-ignore.md` with a documented rationale.

The same command can be reproduced locally (see the header comment in `.github/workflows/openapi-governance.yml`).

### 6.3 Quality Standards

*   **Required fields**: `path`, `method`, `summary`, `description`, `tags`, `responses`, `requiresAuth`.
*   **Recommended practice**: provide `responseExamples` for every response code, and fully define each parameter's `type` and `location`.

### 6.4 Troubleshooting

*   **Swagger UI inaccessible**: check whether static assets are shipped with the server and confirm that static file serving is enabled.
*   **Registration validation fails**: run the `fulla-tests -r OpenApiGenerator` unit test and inspect the specific registration errors.
*   **Governance-gate false positive**: verify that the `oasdiff breaking` output and the exemption-list entry refer to the same path/operation.
