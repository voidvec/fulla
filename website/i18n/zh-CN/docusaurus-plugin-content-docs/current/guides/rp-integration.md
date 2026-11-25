# 将 Fulla 作为 OIDC 提供方接入（RP 指南）

面向把外部依赖方（RP）——WeKnora、Nextcloud、内部 SSO 应用——接入 Fulla
这个身份提供方的团队。本指南汇总那些原本散落在
[build-an-app.md](build-an-app.md)、[配置指南](../operate/configuration-guide.md)
与 [deployment.md](../operate/deployment.md) 中的接入关键事实。它写于 WeKnora
v0.9.0 接入（2026-11）之后，其中的每一条坑，都是真实绊倒过接入方的问题。

## 0. 部署前提（请先阅读）

- **当 SPA 与后端不在同一个 origin 上时，要告诉后端登录页和授权确认页在哪。**
  `/oauth2/authorize` 会把未登录用户送往登录页，把首次授权送往授权确认页。默认
  情况下这两个目标都是后端相对路径（`/login`、`/consent`），只有在网关把前后端放到
  同一个 origin 上时才会落到 SPA（生产栈、Helm 即是如此）。没有这种网关时——开发
  用的 compose 文件，或 SPA 有自己的主机名 / 端口的任何部署——请把
  `FULLA_OAUTH2_LOGIN_URL` 与 `FULLA_OAUTH2_CONSENT_URL` 设为 SPA 的对应页面；
  `deploy/docker/docker-compose.yml` 已经这样做了。在那类部署里若不设置，用户会
  落到后端自己的登录页，需要授权确认时则落到一个 404（见配置指南 §4c）。
- **生产环境请使用 HTTPS。** 在非 localhost 主机上使用明文 HTTP 属于非安全浏览器
  上下文：那里的 `crypto.subtle` / `crypto.randomUUID` 不可用。随附的 SPA 会优雅
  降级（PKCE 改用纯 JS 的 SHA-256，UUID 改由 `getRandomValues` 生成），因此在
  以局域网 IP 通过 HTTP 访问的部署上登录仍然可用；但会话 cookie 是明文传输的——
  请把 HTTP 仅当作局域网内的权宜之计，绝不能作为生产环境的常态。
- **同源反向代理**：SPA 通过相对路径（`/oauth2/…`、`/api/…`、`/.well-known/…`）
  经 nginx 反代访问后端。请让 `VITE_API_BASE_URL` 保持为空；设置它会破坏路由
  （见 deployment.md 的"前端配置"）。

## 1. 注册 RP 客户端

使用管理控制台（应用）或 `POST /oauth2/register` / `POST /api/admin/clients`——
见 [build-an-app.md §1](build-an-app.md)。与接入息息相关的字段：

| 字段 | 注意事项 |
|---|---|
| `redirect_uris` | 授权时精确字符串匹配——协议、主机、端口、路径都必须一致。`localhost` 与 `127.0.0.1` 是不同的主机。不匹配 ⇒ 3027 `VALIDATION_CLIENT_REDIRECT_URI_NOT_REGISTERED`。 |
| `client_type` | `PUBLIC`（SPA / 移动端，无密钥）或 `CONFIDENTIAL`（服务端）。 |
| `token_endpoint_auth_method` | `CONFIDENTIAL` 默认为 `client_secret_basic`。如果你的 OIDC 库把密钥放在表单 body 里发送（很常见：WeKnora 以及许多 PHP/Java 库），请在注册时声明 `client_secret_post`——或者之后通过 `PUT /api/admin/clients/{id}` 修改。F-017 在令牌 / 内省 / 撤销端点严格按声明强制；方式不匹配时即使凭据正确也会返回 `401 invalid_client`。 |

**客户端生命周期**：运行时以数据库为准；`config.json` 的 `clients` 条目仅用于
播种（`ON CONFLICT DO NOTHING`——在那里修改不会作用到已存在的行，重启也不会
回退通过管理 API 做的修改）。见 deployment.md 的"客户端生命周期"。

## 2. 接入流程

把 OIDC 库指向 discovery：

```
{issuer}/.well-known/openid-configuration
```

支持的登录流程是授权码 + PKCE。默认对所有授权码客户端强制 PKCE
（`auth.require_pkce`）。该键由 `require_pkce_for_public` 更名而来，旧名不再被
读取：如果某个部署曾用旧键为旧版 RP 关闭 PKCE，必须改名，否则 PKCE 会重新被强制。
`state` 是**必填**的，8–512 个字符，不能含 URL 分隔符——否则服务端会在任何会话
检查之前直接返回 400。

- 未登录 ⇒ 302 到 SPA 登录页，授权上下文随 query 一并带过去；登录后 SPA 续接
  `/oauth2/authorize`，首次访问会停在 SPA 的授权确认页。
- **IdP 会话与用户的令牌是两个独立的生命周期。** Fulla 的单点登录会话寿命由
  `session_timeout` / `session_max_age` 决定（默认一小时），不会因令牌刷新而延长。
  会话一旦结束——或者你的请求带了 `prompt=login` 或较短的 `max_age`——用户需要
  再登录一次，随后授权请求会自行续接；因此回访用户看到的是 IdP 登录页，而不是
  静默重定向回你的回调地址。如果对你的部署来说这太频繁，请调高这两项配置
  （见[会话管理](../domains/session-management.md)）。
- 授权确认页提交 `POST /oauth2/consent` 时**带 `json=true`**，并根据返回 JSON 的
  `location` 字段自行导航。这是有意为之：原生表单 POST 之后的 302 若落到跨源的 RP
  回调地址，会被 Chrome 的 `form-action` 强制拦截（Chrome 把 `form-action` 应用到
  表单提交之后的重定向上，这与 CSP 规范相悖——w3c/webappsec-csp#8）。如果你要
  自建授权确认前端，请照此处理；不要放宽 CSP。

## 3. 旧版语义：直接调用 `/oauth2/login` 会不经授权确认直接签发 code

`POST /oauth2/login` 是 SPA 的两步登录端点。成功后它**直接签发授权码**——从不
评估是否需要授权确认。这是有意保留的旧版两步语义（门户的授权状态在下游解决；
ADR-0014 与处理器注释记录了这一分层约定）。SPA 自己的浏览器流程从不绕过授权
确认：它调用 `/oauth2/login` 只是为了完成会话这一步，随后续接
`/oauth2/authorize`，由后者执行授权确认评估。

如果你直接调用 `/oauth2/login`（curl 脚本、旧版 RP），你使用的就是这份旧版约定：
首次访问**不会**停在授权确认页。不要把交互式 RP 指向 `/oauth2/login`；请使用
`/oauth2/authorize`。

## 4. 故障排查速查表

| 现象 | 原因 | 处理 |
|---|---|---|
| 令牌交换返回 `401 invalid_client`，但凭据是对的 | 声明的 `token_endpoint_auth_method` 与你发送密钥的方式不一致 | 让方式与传输位置保持一致（§1 表格） |
| 登录失败，返回 3027"回调地址未注册" | `redirect_uri` 没有精确注册 | 注册精确的 URI（§1）；SPA 横幅会显示它实际发送的 URI |
| 浏览器控制台：授权确认 POST 被 CSP `form-action` 拦截 | Chrome 对重定向强制 form-action 的行为（§2） | 使用随附 SPA 的授权确认页（json=true）——不要放宽 CSP |
| SPA 登录提示"客户端错误" / 局域网 HTTP 下 PKCE 失败 | 非安全上下文（§0） | 降级后可用；生产环境请上 HTTPS |
| 卡在"授权中……"，不跳转 | 旧的 SPA 产物（2026-11 之前）仍在用原生表单 POST + 302 | 重新构建 / 部署当前的 SPA 镜像 |
| 隔一段时间后再从 RP 登录，落到了 IdP 首页而不是回调 RP | IdP 会话已结束，而旧的 SPA 产物把仍处于登录态的门户用户从 `/login` 弹到了首页，授权参数随之丢失 | 重新构建 / 部署当前的 SPA 镜像；之后用户登录一次，授权流程即续接（§2） |
| 授权确认 POST 返回 400 `VALIDATION_INVALID_INPUT`"consent_csrf" | 一次性的 CSRF nonce 已过期（TTL 约 10 分钟）或已被消费（重复提交 / 刷新） | 重新发起授权流程 |
| 已签发 code，但令牌交换返回 `invalid_grant` | PKCE verifier 不匹配，或 code 已被消费 / 已过期 | 发送与该流程 `code_challenge` 配对的 verifier（RFC 7636 §4.6） |

## 5. 检查清单

**上线前**

- [ ] 后端与 SPA 经网关同源，或 `FULLA_OAUTH2_LOGIN_URL` / `FULLA_OAUTH2_CONSENT_URL` 指向 SPA 的对应页面
- [ ] issuer 为 `https://`（配置指南 §4）
- [ ] RP 客户端已注册精确的重定向 URI 与正确的认证方式
- [ ] 密钥存放在密钥管理服务中（client_secret 只显示一次）
- [ ] 全链路 HTTPS；代理层启用 HSTS

**RP 无法登录时**

1. 查看响应信封中的确切错误码（3027、`invalid_client` 还是 `invalid_grant`）。
2. 把失败请求的 `redirect_uri` 与客户端记录逐字节比对（`GET /api/admin/clients/{id}`）。
3. 确认客户端密钥的传输位置与声明的认证方式一致。
4. 用信封里返回的 request_id 去 grep 服务端日志。
