---
sidebar_position: 6
---

# 会话管理

fulla 有**两个常被混为一谈的生命周期**：浏览器 SSO 会话（cookie 背后的服务端
会话）与 API 令牌（完全不涉会话）。本页讲清两者、它们如何终结，以及运维上
最重要的部分——Drogon 会话层在机器流量下的代价与调尺寸。

## 1. 一个系统，两个生命周期

| | SSO 会话 | API 令牌 |
|---|---|---|
| 谁拥有 | 走交互式登录/同意流程的浏览器 | 任何调用 token/introspect/userinfo 的客户端——**从不携带 cookie** |
| 载体 | Drogon 服务端会话（`session_timeout`） | 不透明令牌，落库为哈希（[令牌生命周期](token-lifecycle.md)） |
| 终结方式 | `end_session` / 登出 / 空闲过期 | 过期、撤销、refresh 家族级联 |

机器流量不触会话存储。但——下面是关键细节——会话开启时它仍会*创建*会话条目。

## 2. 必须了解的 Drogon 会话行为（上游 #278）

`enable_session: true` 时，Drogon 框架层为**每个不带会话 cookie 的请求**创建
一个 Session 并在 SessionManager 中持有到 `session_timeout` 到期
（[drogon#278](https://github.com/an-tao/drogon/issues/278)，本仓 2026-08-22
实测验证）。API 客户端（token / introspect / userinfo / discovery）从不发送
cookie，因此**每个请求**都付这份代价。

生产 LTO 构建实测（三场 60 秒 c=128 风暴）：

| 量 | 值 |
|---|---|
| 每请求留存 | **~750 B**（三场分别 744/755/759 B） |
| 稳态公式 | `API_QPS × session_timeout × 750 B` |
| discovery 吞吐税 | **~-54%**（session OFF/ON 交错 6 轮：164.6k → 76.3k QPS） |

该税影响所有端点（会话创建发生在框架层、先于路由）。历史基准数字
（S1 87–104k）均为**开启**会话的测量值；无会话真天花板 ~165k。

### 尺寸速查表

按公式推算（交互登录写→读间隔为毫秒级，正确性从不依赖 TTL——S4
登录/authcode 全阶梯已在 120 秒下验证）：

| API QPS（无 cookie） | TTL 3600（默认） | TTL 300 | TTL 120 | TTL 30 |
|---|---|---|---|---|
| 100 | ~0.3 GB | ~23 MB | ~9 MB | ~2 MB |
| 1,000 | **~2.7 GB** | ~225 MB | ~90 MB | ~23 MB |
| 10,000 | **~27 GB（OOM 区）** | ~2.2 GB | ~0.9 GB | ~225 MB |

**指引**：

- 以交互为主（API 低于 100 QPS）的部署：保持默认 3600 秒——完整 SSO 体验不受影响。
- API 流量可观的部署：把 `session_timeout`（与 `session_max_age` 同步）调到
  内存预算可承受的档位。2 分钟空闲窗口对浏览器 SSO 可接受（OIDC 部署常用
  5–15 分钟）。
- 基准档采用 30 秒（`config.bench.json`，`QPS × 30 × 750 B` 封顶）。
- 吞吐税（而非留存）与 TTL 无关、开 session 即存在；结构性修复需上游惰性/
  按路径建会话——跟踪
  [drogon#278](https://github.com/an-tao/drogon/issues/278)。

部署侧运维摘要：[生产部署 · 性能调优](../operate/deployment.md)。

## 3. 终结会话

### RP-Initiated Logout（F-027）——`GET/POST /oauth2/end_session`

终结服务端会话并（可选）重定向。规则：

- `post_logout_redirect_uri` **必须**是客户端已注册的 redirect URI 之一；
  客户端由 `id_token_hint` 的 `aud` 标识。
- hint 的**签名会被验证**（RS256 + kid + iss/exp/sub 策略）。验证失败 →
  400 `AUTH_INVALID_ID_TOKEN_HINT`（4006）。
- 无合法 hint + 已注册 URI → 400。成功：带 `state` 回显 302，未提供重定向
  URI 时返回 200。

### API 登出——`POST /oauth2/logout`

撤销所提交的令牌**并**调用 `session()->clear()`（F-028），服务端会话与
access token 一同终结。

### 重新认证语义（F-022）

`prompt=login` 强制重新认证（即使会话存活）；`prompt=none` 禁止 UI
（`login_required` / `consent_required` 错误重定向回已验证的 redirect URI）；
`max_age=<秒>` 在会话的 `auth_time` 过老时强制重新认证。`auth_time`、`amr`、
`acr`（1 = 密码，2 = MFA）随授权码传递并打进 id_token——见
[配置指南 §6](../operate/configuration-guide.md)。

### SSO 会话结束后，从依赖方发起登录会发生什么

令牌刷新不会延长 SSO 会话。access / refresh 令牌对在其自身的生命周期内持续有效，
但 SSO 会话一旦结束（空闲过期或到达 `session_max_age`），`/oauth2/authorize` 就会
重新把用户视为未登录，并将其送往登录页，同时把授权请求原样保留在 URL 上——
`prompt=login` 或 `max_age` 超期时的行为相同。用户再登录一次（得到一个带有全新
`auth_time` 与 `amr` 的 SSO 会话），授权流程随即续接，依赖方仍能拿到它的响应。
这就是为什么仍登录着门户的用户，在隔天回到某个应用、由该应用发起登录时，会被
要求重新登录。

## 4. 会话不是什么

- 令牌撤销面（按令牌 / 客户端 / 用户撤销）作用于**令牌**而非 SSO 会话——见
  [令牌生命周期 §6](token-lifecycle.md)。
- 管理台的令牌浏览器列出的是落库令牌，不暴露在线会话清单。
