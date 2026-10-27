# 在 Fulla 上构建应用（OIDC 登录接入）

让您的 Web / 移动 / CLI 应用使用 Fulla 作为身份提供方：标准 OpenID Connect，
任何合规库即插即用，无自定义协议。

## 1. 注册您的应用

- **已启用自助注册的部署**：门户 → **我的应用** → *注册应用*。选择：
  - `PUBLIC`：浏览器 / 移动应用（授权码 + PKCE，无密钥）；
  - `CONFIDENTIAL`：服务端应用（客户端密钥，**仅显示一次**——请存入密钥管理服务）。
- 其它部署请联系 Fulla 管理员创建客户端。
- 登记精确的回调 URI（开发环境外必须 `https://`）。
- 从自助白名单中选择 scope（默认 `openid`、`profile`、`email`）。

### 1a. 编程式注册（RFC 7591）

自动化与 Terraform 式供给可以直接调 `POST /oauth2/register` 注册客户端。
该端点**仅管理员可用**：Bearer 令牌须属于持有 `admin` 角色的用户
（开放注册是刻意的非目标）。管理控制台中的"注册应用（RFC 7591）"表单
调用的就是它。

```bash
# 1) 获取管理员 Bearer 令牌。全新开发环境用种子 fulla-portal PUBLIC
#    客户端与引导管理员即可完整走通：
VERIFIER=$(openssl rand -base64 48 | tr '+/' '-_' | tr -d '=')
CHALLENGE=$(printf %s "$VERIFIER" | openssl dgst -sha256 -binary | base64 | tr '+/' '-_' | tr -d '=')
CODE=$(curl -s -X POST 'http://localhost:5555/oauth2/login?json=true' \
  -d "username=admin&password=ADMIN_PASSWORD" \
  -d "client_id=fulla-portal&redirect_uri=http://localhost:5173/callback" \
  -d "scope=openid&state=dcr&code_challenge=$CHALLENGE&code_challenge_method=S256" \
  | jq -r .code)
TOKEN=$(curl -s -X POST http://localhost:5555/oauth2/token \
  -d "grant_type=authorization_code&code=$CODE" \
  -d "client_id=fulla-portal&redirect_uri=http://localhost:5173/callback" \
  -d "code_verifier=$VERIFIER" | jq -r .access_token)

# 2) 注册客户端：
curl -X POST http://localhost:5555/oauth2/register \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "client_name": "my-terraform-managed-app",
    "client_type": "CONFIDENTIAL",
    "token_endpoint_auth_method": "client_secret_basic",
    "redirect_uris": ["https://app.example.com/callback"],
    "grant_types": ["authorization_code", "refresh_token"],
    "post_logout_redirect_uris": ["https://app.example.com/"]
  }'
```

请求字段：

| 字段 | 必填 | 说明 |
|---|---|---|
| `client_name` | 是 | 展示名，显示在登录 / 授权确认页。 |
| `client_type` | 否 | `CONFIDENTIAL`（默认）或 `PUBLIC`。 |
| `token_endpoint_auth_method` | 否 | `none` / `client_secret_basic` / `client_secret_post`。按类型默认：PUBLIC → `none`（唯一选项），CONFIDENTIAL → `client_secret_basic`。之后令牌端点会持续强制（F-017）。 |
| `redirect_uris` | CONFIDENTIAL 必填 | 数组。开发环境外必须 `https://`（回环地址豁免；开发环境可用 `auth.allow_http_redirect_uri` 放开）。授权时精确字符串匹配。 |
| `grant_types` | 否 | `authorization_code`、`refresh_token`、`client_credentials` 的子集；默认 `["authorization_code"]`。 |
| `post_logout_redirect_uris` | 否 | 专用登出目标（同套 scheme 策略）。非空时对 `end_session` **权威生效**——登录合法但未登记为登出目标的 URI 会被拒绝；留空则回退到登录回调列表。 |

`201 Created` 响应回显注册内容并附加生成的凭据：`client_id`、
`client_secret`、`client_id_issued_at`、`client_secret_expires_at`
（`0`——密钥不会自动过期，需显式轮换）。**`client_secret` 仅显示一次**——
请立即存入密钥管理服务；没有读回路径，只有重置 / 轮换。注册动作以
`client_registered` 计入审计（成功与失败皆记）。

已有注册的管理（RFC 7592 读取 / 更新 / 删除）未实现——请改用管理端
客户端 API（`/api/admin/clients`）。

## 2. 将 OIDC 库指向 Fulla

Discovery 一步到位：

```
https://your-fulla.example/.well-known/openid-configuration
```

标准配置：授权端点 `/oauth2/authorize`、令牌端点 `/oauth2/token`、
JWKS `/.well-known/jwks.json`、scope `openid profile email`。

## 3. 用户登录

授权码 + PKCE（PUBLIC 与 CONFIDENTIAL 客户端均可）：

```
GET /oauth2/authorize?
  client_id=YOUR_CLIENT_ID
  &redirect_uri=https://your-app.example/callback
  &response_type=code
  &scope=openid profile email
  &state=<random>
  &code_challenge=<S256(verifier)>
  &code_challenge_method=S256
```

在 `/oauth2/token` 交换 code，用 JWKS 验证 `id_token`，
并从 `/oauth2/userinfo` 读取资料 claims。

## 4. 自动化（无用户在场）

脚本与服务间集成不走用户登录——设备流与 client_credentials 见
[使用 Fulla 做自动化](automation.md)。

## 5. 运维要点

- 轮换密钥（门户 → 我的应用 → *轮换密钥*）会使旧密钥立即失效。
- 删除应用会吊销其用户授权并停止令牌签发。
- 管理员可停用滥用应用；停用后的客户端校验立即失败。
