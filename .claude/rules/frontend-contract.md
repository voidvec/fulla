---
description: 前后端契约事实清单——写前端或改对应后端契约前必须核对的 load-bearing 事实（实测踩坑而来）
globs:
  - "frontends/**"
  - "apps/server/openapi.yaml"
---

# 前后端契约事实（实测裁定，写码前核对）

这些事实来自 2026-08/09 的前后端全量差距分析与修复（PR #140/#141/#156 等），**与直觉相反的
地方已用代码核实**。改后端对应行为时必须评估前端影响，反之亦然。

## 认证与 token

- **PKCE 对 PUBLIC 客户端强制**（全部 4 份 config + 代码默认 `require_pkce_for_public=true`）。
  任何登录/authorize 流程的前端调用必须带 `code_challenge`；`state` 须 8–512 字符。
- **PKCE verifier 只能在 MFA 验证成功后清除**：`/oauth2/token` 对带 challenge 的 code 拒绝空
  verifier——进门就清 = 错一次码后重试必死。
- **user 前端 access_token 只存内存**（`http.ts` 只持久化 refresh_token）。任何整页跳转往返
  （OAuth 回调）后内存 token 必丢——回调页必须先 `tryRestoreSession()` 再判断登录态；
  e2e 断言 `localStorage.access_token` 永远是 null。
- **同一 access token 永不同时发撤销端点和需要它认证的端点**（logout 并发 revoke 会 401 掉
  session 清理与 backchannel 通知；user 登出走 POST `/oauth2/logout` Bearer 单点）。
- **vue-client 种子 redirect_uri 是 `http://127.0.0.1:8080/callback`**——精确匹配，localhost
  被拒。
- GitHub 社交登录：**v1.4.0 起**前端不再用 `VITE_GITHUB_CLIENT_ID`（已废除）。登录页从公开端点
  `GET /api/auth/providers` 拉取已启用 provider 与服务端拼好的 `authorize_url`（redirect 解析 =
  per-provider `redirect_uri` 覆盖优先，否则 `frontend.url` + `/callback/{provider}`）；可用性 =
  档位开关（`external_auth.tiers.domestic/international`，env 别名 `FULLA_EXTERNAL_TIER_*`，
  国内档管 WeChat、国外档管 GitHub/Google）与凭据配置同时成立。回调路径不变：登录/绑定共用
  `<门户origin>/callback/{provider}`。

## 端点行为口径

- `/oauth2/mfa/verify`：必填 `mfa_token+code+client_id+redirect_uri`；错码返回
  `AUTH_INVALID_CREDENTIALS`（未知错误码前端兜底"发生未知错误"）。
- `/oauth2/device/approve`：**form-urlencoded 不是 JSON**（`req->getParameter`），必填
  `user_code+user_id`；rbac 规则缺失时 AuthorizationFilter 默认 DENY（含 admin 全员 403）——
  新增受管端点要核对 5 份 config 的 rbac 规则。
- `/health/ready` 值域：status `{ok,degraded,unhealthy}`、database
  `{connected,not_configured,disconnected,unavailable}`、redis
  `{connected,not_configured,disconnected}`；**not_configured 是健康态**，前端三态渲染别当错误。
- `oauth2_device_codes.user_code` 是 **VARCHAR(8)**（测试种子勿超长）。
- 双主体形态：session `"userId"` = 内部 id 字符串，`"sub"` = 公开 subject；`oauth2_codes.user_id`
  存在双形态（login/MFA 发的绑 public sub）。前端透传 user_id 类参数时不要自作主张换算。

## 前端工程口径

- admin SPA 挂 `/admin/` 子路径（**`/admin/dashboard` 是未注册路由**，仪表盘在 `/admin/` 根）；
  admin API 前缀一律 `/api/admin/*`。
- 语义样式 token：`error-*` / `brand-*` / `bg-surface`（勿写 `red-*`/`sky-*`/`bg-white`）；
  暗色块载体 = App.vue 非 scoped style + `html[data-theme="dark"]`（其它三种载体实测死路）。
- i18n：`services/` 层保持零 vue-i18n 依赖；`i18nKeys.test.ts` 用 `import.meta.glob('?raw')`
  **禁 node:fs**（tsc 构建门会炸）；localStorage 写 locale 必须写完整 `'zh-CN'`（写 `'zh'` 被
  SUPPORTED_LOCALES 拒绝回退 navigator）。
- **测试文件永远不要跨 app import**（user 的镜像上下文只有 `frontends/user/`——跨 app 静态/
  动态 import 会让 docker 用户镜像 TS2307 不可构建）。
- 组件双副本由 `scripts/check-ui-sync.mjs` 守护字节一致；体积门 `scripts/check-frontend-size.mjs`
  必须从 repo 根跑。
- vite dev 默认绑 `[::1]`，harness 用 127.0.0.1——本地起 dev server 必须
  `npm run dev -- --host 127.0.0.1`。

## 发现后端 bug 时

前端 mock 固化了错误契约也必须同步修（PR #140 教训：修 bug 不修 mock = e2e 假绿）；但先核对
`docs/domains/api-reference.md` 与 `ErrorCatalog.cc` 的官方口径，mock 以真契约为准。
