# 社交登录指南

GitHub、Google、微信登录都通过服务端交换 code 获取上游身份，再查询本地账号映射，按配置自动创建账号，最后签发第一方 access/refresh token。绑定到锁定或已删除账号时拒绝登录；未绑定且 `external_auth.auto_create_on_first_login=false` 时返回 403 `AUTH_SOCIAL_ACCOUNT_NOT_LINKED`，不会创建账号。

## 浏览器绑定的一次性交易

1. `GET /api/auth/providers` 发现启用的 provider。兼容字段 `authorize_url` 已弃用，始终为空，不能用于跳转。
2. 以准确配置的 `Origin`、可保存 Cookie 的客户端 POST `{}` 到 `/api/auth/providers/{provider}/authorize`，无需 Bearer。浏览器使用 `withCredentials: true` 或 fetch `credentials: 'include'`。
3. 无绑定 Cookie 时返回 **204** 并设置 Cookie，不签发 state。携带该 Cookie 重试一次；若仍为 204，停止流程并提示启用 Cookie，禁止循环重试。
4. **200** 返回 `authorize_url`、`state`、`redirect_uri`、`expires_in: 600`。按 state 在本标签页 sessionStorage 保存 `{purpose:'login',provider,redirectUri,createdAt}`，成功后才跳转服务端返回的 URL。
5. 在创建路由前移除回调 URL 中的 code、state、error；在内存中读取一次回调参数，验证本地交易的用途、provider、回调 origin/path 和 600 秒有效期。不接受外部直接发起、无本地上下文的回调。
6. 用相同 Origin 和 Cookie POST `{code,state}` 到 `/api/{provider}/login`。只接受 JSON 或表单正文，不接受查询参数；重复字段（含 URL 解码后的同名字段）拒绝。服务端先验证并原子消费 state，再访问上游或修改账号。交换 POST 无论网络超时还是上游失败都不自动重试，必须重新发起交易。

state 和绑定 Cookie 分别使用 32 字节 CSPRNG，编码为 64 位小写十六进制。Redis key 为 `fulla:social:login-state:<sha256(state)>`，值绑定 provider、上游 client ID、固定 redirect URI、绑定摘要和用途 login。通过 `SET NX EX 600` 预留，用 Lua 先比较再删除；错误浏览器或 provider 不消费合法交易。无生产内存回退或无状态回退。Redis 操作五秒后返回失败并抑制晚到回调；超时后的命令仍可能完成，因此客户端必须重新开始。

| 条件 | 状态码 / 错误码 |
| --- | --- |
| 缺少 code/state | 400 `VALIDATION_MISSING_REQUIRED_FIELD` |
| 正文或 Cookie 无效、state 过期/错配/重放 | 400 `VALIDATION_INVALID_INPUT` |
| 缺少/未允许 Origin，或明确跨站请求 | 403 `AUTHZ_ACCESS_DENIED` |
| provider 未知或禁用 | 404 `VALIDATION_RESOURCE_NOT_FOUND` |
| 存储或随机数不可用 | 500 `INTERNAL_ERROR` |

## 配置与部署边界

`external_auth.tiers.domestic` 管微信，`international` 管 GitHub/Google，默认均为 true；档位开启且凭据有效才启用。空值、`YOUR_`、`OVERRIDE_` 模板凭据均视为未配置。GitHub/Google 使用 `client_id/client_secret`，微信使用 `appid/secret`。Google 回调 `/callback/google`，GitHub `/callback/github`，微信 `/callback/wechat`；微信上游要求公网域名。

回调取 `external_auth.{provider}.redirect_uri`，否则取 `frontend.url + /callback/{provider}`。各启用 provider 必须使用不同的规范 origin/path，只有 query 不同不算独立回调；禁止编码、点段和歧义路径。生产要求 HTTPS；须与上游注册 URI 完全一致。Google 授权与 code 交换使用同一解析结果。

在 `custom_config.cors.allow_origins` 配置准确门户 Origin，禁止通配符。门户与 API 必须同站，以便 Lax Cookie 随交换 POST 发送，推荐同一 HTTPS origin；跨站门户/API 不受支持，开启 credentials 无法绕过 SameSite。

生产 Cookie：`__Host-fulla-social-binding; Secure; HttpOnly; SameSite=Lax; Path=/; Max-Age=600`，无 Domain。多次签发续期同一个绑定值，保留并行交易。仅非生产且配置 HTTP loopback 门户时可用无 Secure 的 `fulla-social-binding-dev`；从 .env 或环境变量加载的 `FULLA_ENV=production` 始终禁用该例外。不能配置明文公网门户。

## 绑定、token 与日志

账号绑定继续使用独立的已认证服务端 state。安全页跳转前保存 `purpose:'link'` 的本地交易，回调先恢复登录会话再调绑定端点。不能用“有 state”推断绑定用途。升级前已在途、缺少本地上下文的回调应重新发起。access token 仍只存内存；不会新增持久化 access token。

签发、交换及回调文档均禁止缓存。内置用户端和网关 Nginx 访问日志不包含 query 或 Referer，回调 location 禁止错误日志转储请求，并保持 `Referrer-Policy: no-referrer`。外部 ingress、CDN、APM、链路追踪和浏览器分析仍需独立屏蔽 query、请求正文、Cookie/Set-Cookie 和 token，禁止为排错开启原始请求采集。

这是一次针对登录 CSRF 的安全收紧，不代表完成 OAuth Security BCP、PKCE、issuer 校验或全部账号策略。账号映射、自动创建、第一方客户端 `external_auth.social_token_client_id`（默认 fulla-portal）、scope `openid profile email` 和 `SOCIAL_LOGIN_TOKEN_ISSUED` 审计语义不变；不写虚构 consent 记录，不签发 id_token。

## 升级与回滚（下一 MINOR）

前后端配套部署并清理旧 SPA 缓存，配置准确 CORS、Redis、HTTPS。冒烟检查 204 → 200 → 成功交换、重放拒绝和账号绑定。SDK 调用方须保留 Cookie、发送 Origin，并在正文提交 code/state。失败时禁用社交 provider 或配套向前修复，不能回退无状态登录。本改动无数据库迁移，不创建 tag 或发布版本。
