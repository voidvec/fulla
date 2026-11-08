# 社交登录指南

后端社交登录按"提供方适配器"模式实现。自 #70 起，GitHub、Google、微信三家提供方走**同一条**
闭环：上游 code 交换 → subject 映射查找 →（首次登录）自动创建本地账号 → 签发第一方 token 对。

## 统一账号模型（#70）

三家提供方都经 `oauth2_subject_mappings` 把 `(provider, subject)`——GitHub 数字 id / Google `sub` /
微信 `openid`——映射到本地用户：

- **已有映射**：为已绑定的本地用户签发 token（已绑定用户若被软删除或锁定，一律以通用认证错误拒绝——
  不泄露账号状态）。
- **无映射 + 自动创建开启**（默认）：创建本地账号（用户名 `gh_<login>` / `google_<sub12>` /
  `wx_<openid12>`；冲突时追加随机后缀重试一次，仍冲突则以 `VALIDATION_USERNAME_TAKEN` 失败），
  写入映射行，授予默认 `user` 角色，并签发 token。
- **无映射 + 自动创建关闭**：返回 `403 AUTH_SOCIAL_ACCOUNT_NOT_LINKED`，且**不产生**任何账号副作用。
  用 403（而非 401），是因为"未绑定"是客户端可据以行动的授权状态（引导用户走绑定流程）；探测需要
  目标账号本人的上游提供方 code，因此不存在跨用户枚举向量。

自动创建开关是**全局**的——`external_auth.auto_create_on_first_login`（默认 `true`）同时管
GitHub/Google/微信；它是社交登录策略，不是按提供方的开关。已绑定的用户不受该开关影响。

## 第一方 token 签发（#70）

登录端点经共享的 `SocialTokenIssuer` 签发一对不透明的 access/refresh token：

- token 行存的是**平台主体**（`users.public_sub`）——与所有 Bearer 认证 handler 解析出的值相同
  （`/api/me`、改密、MFA、WebAuthn）。（GitHub 以前存的是内部数字 id，导致其 token 在每个需认证的
  端点上都返回 404——随 issuer 抽取一并修复。）
- 签发给配置的**第一方** client：`external_auth.social_token_client_id`（默认 `fulla-portal`）。
  该键绝不能指向第三方 client——这次签发没有同意交互，指过去就等于把没有人同意过的 token 交给那个 client。
- scope 为 `openid profile email`。注意：`openid` 这个 scope 值在本端点上不承载 OIDC 语义（不签发
  id_token）；保留它只是为了与第一方 client 的 scope 集合保持一致。

### 为什么记审计事件而不写同意记录

签发时记录一条 `SOCIAL_LOGIN_TOKEN_ISSUED` 审计动作（provider、client、scope、内部 id），而不是写一行
`oauth2_user_consents`：同意行会悄无声息地满足同意页的 Tier-3 检查，让配置的 client 在用户从未被询问过的
scope 上被预先批准——这是伪造的同意证据。显式的社交登录同意交互已登记为后续工作；在那之前，审计事件才是
如实的记录。

这是一个第一方扩展端点，不属于 RFC 6749 的四种授权类型——GitHub 流程一直处于同样的位置，如今补上了文档
与审计追踪。走标准轨道的替代方案（社交登录先建立浏览器会话，再由 SPA 用免同意的第一方 client 跑
授权码 + PKCE）已登记为后续工作。

## 提供方分级（v1.4.0）

不同部署适用的外部提供方不同（GitHub/Google 在中国大陆不可达；微信只面向国内）。可用性是一道
服务端门，由 `config.json` 中 `custom_config` 下的两个开关控制（环境变量别名
`FULLA_EXTERNAL_TIER_DOMESTIC` / `FULLA_EXTERNAL_TIER_INTERNATIONAL`）：

```json
"external_auth": {
    "tiers": {
        "domestic": true,       // 控制微信
        "international": true   // 控制 GitHub + Google
    }
}
```

实际启用 = 分级开启 **且** 凭据有效。登录页经 `GET /api/auth/providers` 发现启用的提供方；
兼容字段 `authorize_url` 已弃用且为空，不能用于跳转。发起登录必须使用下述交易 POST。
两个分级默认均为 `true`；未配置或 `YOUR_` / `OVERRIDE_` 模板凭据不会启用提供方。

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

### 配置与部署边界

`external_auth.tiers.domestic` 管微信，`international` 管 GitHub/Google，默认均为 true；档位开启且凭据有效才启用。空值、`YOUR_`、`OVERRIDE_` 模板凭据均视为未配置。GitHub/Google 使用 `client_id/client_secret`，微信使用 `appid/secret`。Google 回调 `/callback/google`，GitHub `/callback/github`，微信 `/callback/wechat`；微信上游要求公网域名。

回调取 `external_auth.{provider}.redirect_uri`，否则取 `frontend.url + /callback/{provider}`。各启用 provider 必须使用不同的规范 origin/path，只有 query 不同不算独立回调；禁止编码、点段和歧义路径。生产要求 HTTPS；须与上游注册 URI 完全一致。Google 授权与 code 交换使用同一解析结果。

在 `custom_config.cors.allow_origins` 配置准确门户 Origin，禁止通配符。门户与 API 必须同站，以便 Lax Cookie 随交换 POST 发送，推荐同一 HTTPS origin；跨站门户/API 不受支持，开启 credentials 无法绕过 SameSite。

生产 Cookie：`__Host-fulla-social-binding; Secure; HttpOnly; SameSite=Lax; Path=/; Max-Age=600`，无 Domain。多次签发续期同一个绑定值，保留并行交易。仅非生产且配置 HTTP loopback 门户时可用无 Secure 的 `fulla-social-binding-dev`；从 .env 或环境变量加载的 `FULLA_ENV=production` 始终禁用该例外。不能配置明文公网门户。

### 绑定、token 与日志

账号绑定继续使用独立的已认证服务端 state。安全页跳转前保存 `purpose:'link'` 的本地交易，回调先恢复登录会话再调绑定端点。不能用“有 state”推断绑定用途。升级前已在途、缺少本地上下文的回调应重新发起。access token 仍只存内存；不会新增持久化 access token。

签发、交换及回调文档均禁止缓存。内置用户端和网关 Nginx 访问日志不包含 query 或 Referer，回调 location 禁止错误日志转储请求，并保持 `Referrer-Policy: no-referrer`。外部 ingress、CDN、APM、链路追踪和浏览器分析仍需独立屏蔽 query、请求正文、Cookie/Set-Cookie 和 token，禁止为排错开启原始请求采集。

这是一次针对登录 CSRF 的安全收紧，不代表完成 OAuth Security BCP、PKCE、issuer 校验或全部账号策略。账号映射、自动创建、第一方客户端 `external_auth.social_token_client_id`（默认 fulla-portal）、scope `openid profile email` 和 `SOCIAL_LOGIN_TOKEN_ISSUED` 审计语义不变；不写虚构 consent 记录，不签发 id_token。

### 升级与回滚（下一 MINOR）

前后端配套部署并清理旧 SPA 缓存，配置准确 CORS、Redis、HTTPS。冒烟检查 204 → 200 → 成功交换、重放拒绝和账号绑定。SDK 调用方须保留 Cookie、发送 Origin，并在正文提交 code/state。失败时禁用社交 provider 或配套向前修复，不能回退无状态登录。本改动无数据库迁移，不创建 tag 或发布版本。

## GitHub（已完整接线，主线）

- 后端路由：`POST /api/github/login`；`frontends/user` 前端依据运行时发现端点
  `GET /api/auth/providers` 渲染"Sign in with GitHub"按钮（v1.4.0 提供方分级——按钮跟随
  服务端 `external_auth` 门，不涉及任何前端构建变量）。
- 回调：`/callback/github`。

## Google（已接线，#70）

1. Google Cloud 侧创建 OAuth 2.0 客户端（Web 应用；已授权的重定向 URI = `<portal origin>/callback/google`）。
2. 后端配置（`config.json`）：

```json
"external_auth": {
    "google": {
        "client_id": "<your-client-id>",
        "client_secret": "<your-client-secret>",
        "redirect_uri": "<portal origin>/callback/google"
    }
}
```

3. 携带相同 Origin 与绑定 Cookie，以 JSON/表单正文 `{code,state}` POST `/api/google/login`，完成 code 交换并返回 token 对（见上文）。
4. 前端：发现端点报告 Google 已启用时，登录页渲染"Sign in with Google"按钮（未配置 = 隐藏）；
   `/callback/google` 路由（通用化的 `SocialCallbackPage`）保存 token 并把用户带到首页。

## 微信（登录已接线；扫码需移动端 user-agent 界面）

1. 微信开放平台创建网站应用（**不支持 localhost 回调**，需备案域名）。
2. 后端配置同上结构（`external_auth.wechat`：appid / app_secret）。
3. 后端路由 `POST /api/wechat/login`——同一闭环；微信不提供邮箱，创建的账号沿用 GitHub 的空邮箱处理方式。
4. 前端：`/callback/wechat` 已存在；桌面 SPA 只在发现端点报告微信已启用时——即国内分级
   （`external_auth.tiers.domestic`，默认开启）**且**已配置凭据——展示一条提示（扫码授权流程需要支持
   微信的设备/浏览器界面，桌面端"按钮 + 重定向"的交互无法完成它；显式的桌面扫码流程已登记为后续工作）。
5. 本地开发三招：hosts 把回调域名指到 127.0.0.1 / Nginx 反代 / 内网穿透。

## 通用安全注意

- 社交回调的 `state` 必须校验（防 CSRF）；提供方返回的 subject 只信任服务端 code 交换结果，
  绝不信任前端提交的用户信息。
- 解绑社交账号有"最后一种登录方式"保护与并发解绑竞态的已知限制（见 `docs/history`
  归档的 social-link 设计与 CHANGELOG #54/#69 修复记录）。

> 合并自已退役的 google-guide.md 与 wechat-guide.md（docs 治理 A2），并修正了 google 指南
> 中"前端无按钮却让用户点击按钮"的自相矛盾。#70 为 Google/微信补齐了闭环，并围绕统一的
> 账号/签发模型重写了本页。
