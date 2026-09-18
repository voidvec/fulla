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
