# OpenID Connect (OIDC) 集成指南

本指南介绍如何将本 OAuth2 服务作为 OIDC Provider 集成到你的应用中。

## 1. Discovery 端点

OIDC Discovery 端点提供 Provider 的所有配置信息：

```
GET /.well-known/openid-configuration
```

返回示例：

```json
{
  "issuer": "https://your-domain.com",
  "authorization_endpoint": "https://your-domain.com/oauth2/authorize",
  "token_endpoint": "https://your-domain.com/oauth2/token",
  "userinfo_endpoint": "https://your-domain.com/oauth2/userinfo",
  "jwks_uri": "https://your-domain.com/.well-known/jwks.json",
  "response_types_supported": ["code"],
  "subject_types_supported": ["public"],
  "id_token_signing_alg_values_supported": ["RS256"],
  "scopes_supported": ["openid", "profile", "email"]
}
```

## 2. JWKS 端点

JSON Web Key Set 端点提供用于验证 `id_token` 签名的公钥：

```
GET /.well-known/jwks.json
```

返回示例：

```json
{
  "keys": [
    {
      "kty": "RSA",
      "use": "sig",
      "alg": "RS256",
      "kid": "default-key-id",
      "n": "<base64url-encoded modulus>",
      "e": "AQAB"
    }
  ]
}
```

## 3. id_token 格式

`id_token` 是一个 RS256 签名的 JWT，包含以下标准 claims：

| Claim | 描述 | 示例 |
|-------|------|------|
| `iss` | 签发者 (Issuer) | `https://your-domain.com` |
| `sub` | 用户唯一标识 (UUID public_sub) | `550e8400-e29b-41d4-a716-446655440000` |
| `aud` | 受众 (Client ID) | `your-client-id` |
| `exp` | 过期时间 (Unix timestamp) | `1700000000` |
| `iat` | 签发时间 (Unix timestamp) | `1699996400` |
| `nonce` | 请求中传入的 nonce 值 | `abc123` |

根据请求内容，还可能包含：

- **认证上下文**: `auth_time`（可用时始终携带）、`amr`（如 `["pwd", "mfa"]`）、`acr`（`"1"` 仅密码 / `"2"` MFA）
- **org scope 且携带组织上下文**（v1.4.0）：`org_ctx` —— 表示本次授权绑定的活跃组织与调用者当前成员角色的对象 `{ org_id, org_name, roles[] }`；仅在授权请求携带有效 `org_id` 且 client 申请了 `org` scope 时出现，签发时实时复查成员资格。

profile 与 email 类 claims（`name`、`preferred_username`、`email`、`email_verified`）**不会**放入 id_token——请通过 userinfo 端点并携带对应 `profile` / `email` scope 获取（见 §5）。

## 4. 验证 id_token

### 4.1 验证步骤

1. **解码 JWT Header**：提取 `kid`（Key ID）和 `alg`（应为 RS256）
2. **获取公钥**：从 JWKS 端点获取对应 `kid` 的公钥
3. **验证签名**：使用 RSA 公钥验证 JWT 签名
4. **验证 Claims**：
   - `iss` 必须匹配你配置的 Issuer URL
   - `aud` 必须包含你的 Client ID
   - `exp` 必须大于当前时间
   - `nonce` 必须匹配你在授权请求中发送的值

### 4.2 安全注意事项

- **始终验证签名**，不要信任未验证的 JWT
- **缓存 JWKS** 但设置合理的刷新间隔（建议 24 小时或按 Cache-Control 头）
- **检查 `alg` 头**，拒绝 `none` 算法（防止算法降级攻击）

## 5. 支持的 Scopes 与 Claims

| Scope | 返回的 Claims |
|-------|---------------|
| `openid` | `sub` (必需 scope，启用 OIDC) |
| `profile` | `name`, `preferred_username`（经 userinfo 端点返回；旧 `username` key 同时下发至 v1.6.0 移除） |
| `email` | `email`, `email_verified`（经 userinfo 端点返回） |
| `org` | `org_ctx`（`{org_id, org_name, roles[]}`）—— id_token 与 userinfo；仅当授权携带有效组织上下文（v1.4.0） |

## 6. 集成示例

### 6.1 使用标准 OIDC 客户端库 (Node.js)

```javascript
const { Issuer } = require('openid-client');

// 自动发现 Provider 配置
const issuer = await Issuer.discover('https://your-domain.com');

const client = new issuer.Client({
  client_id: 'your-client-id',
  client_secret: 'your-client-secret',
  redirect_uris: ['http://localhost:3000/callback'],
  response_types: ['code'],
});

// 生成授权 URL
const authUrl = client.authorizationUrl({
  scope: 'openid profile email',
  state: 'random-state-value',
  nonce: 'random-nonce-value',
});

// 处理回调
const params = client.callbackParams(req);
const tokenSet = await client.callback('http://localhost:3000/callback', params, {
  state: 'random-state-value',
  nonce: 'random-nonce-value',
});

console.log('ID Token claims:', tokenSet.claims());
console.log('Access Token:', tokenSet.access_token);
```

### 6.2 使用 Python (authlib)

```python
from authlib.integrations.requests_client import OAuth2Session

client = OAuth2Session(
    client_id='your-client-id',
    client_secret='your-client-secret',
    redirect_uri='http://localhost:8000/callback',
    scope='openid profile email'
)

# 获取授权 URL
uri, state = client.create_authorization_url(
    'https://your-domain.com/oauth2/authorize'
)

# 处理回调，换取 Token
token = client.fetch_token(
    'https://your-domain.com/oauth2/token',
    authorization_response=callback_url
)

# 获取用户信息
userinfo = client.get('https://your-domain.com/oauth2/userinfo').json()
```

### 6.3 使用 Go (coreos/go-oidc)

```go
provider, err := oidc.NewProvider(ctx, "https://your-domain.com")

oauth2Config := oauth2.Config{
    ClientID:     "your-client-id",
    ClientSecret: "your-client-secret",
    RedirectURL:  "http://localhost:8080/callback",
    Endpoint:     provider.Endpoint(),
    Scopes:       []string{oidc.ScopeOpenID, "profile", "email"},
}

// 验证 id_token
verifier := provider.Verifier(&oidc.Config{ClientID: "your-client-id"})
idToken, err := verifier.Verify(ctx, rawIDToken)
```

## 7. 常见问题

**Q: id_token 的有效期是多久？**
A: 与 Access Token 一致，默认 1 小时。

**Q: 如何处理密钥轮转？**
A: 定期从 JWKS 端点刷新公钥。当验证失败时，先尝试刷新 JWKS 再重试验证。

**Q: 是否支持 PKCE？**
A: 支持。PKCE (RFC 7636) 已实现并默认对 PUBLIC 客户端强制启用（同时支持 `plain` 与 `S256`）。SPA 和移动端客户端应使用 `S256`。
