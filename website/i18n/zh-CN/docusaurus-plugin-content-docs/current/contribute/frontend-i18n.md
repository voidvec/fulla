# 前端 i18n 指南 —— 选型、影响范围与贡献者工作流

> 决策记录：[ADR-0013](../adr/ADR-0013.md)。本指南承载评估证据、影响面清单、带验收标准的落地方案与日常贡献者工作流。本页与英文原版同 PR 双写（文档双写约定）。

## 摘要

- 两个前端通过 **vue-i18n v11** 消息目录提供**英文（默认）与简体中文**——一个切换器同时驱动页面骨架*和*错误信息。
- **translate.js 等运行时机翻组件被拒绝**用于产品 UI：它们把页面文本发送到第三方云、在首绘后翻译、无法保证 OAuth 术语准确。机器翻译只能辅助离线翻译工作流，绝不进运行时。
- 新增语言 = 一个目录文件 + 注册（见[贡献者工作流](#贡献者工作流)）。

## 1. 评估对象

### 1.1 translate.js（xnx3/translate）—— 自动页面翻译

| 维度 | 结论 |
|---|---|
| 许可 / 活跃度 | MIT；约 3.1k star；最新 release v4.0.0（2026-02），仓库活跃 —— **嵌入本身许可无冲突** |
| 机制 | 加载后遍历 DOM；把文本节点批量送往作者私有云（`api.translate.zvo.cn`、`america.api…`）再把译文换回页面 |
| 隐私 | **默认把页面文本外发第三方服务。** IdP 页面含用户名、邮箱、错误详情，这一点即否决 |
| 容量 / 可用性 | 免费通道有**每日字数上限**；运行时依赖外部节点；"私有部署"为付费商业授权 |
| UX | 首绘*之后*才翻译（天然 FOUC）；SPA 路由切换依赖 mutation 监听（Vue 相关 issue #54、#94 未决）；改写 `input` value 属性 —— 登录/MFA 表单隐患 |
| 质量 | 无术语控制："consent"、"scope"、"authorization code" 任由引擎猜；漏译/过译混杂 |
| 体积 | 运行时约 47 KB gzip（vue-i18n 约 10–14 KB brotli） |

**裁定：技术上可嵌入，拒绝作为 fulla UI 机制。** 其失败模式（数据外发、离线失效、术语漂移）恰好落在身份提供方承受不起的位置。

### 1.2 Vue 3 策展目录库

| 选项 | 适配度 | 说明 |
|---|---|---|
| **vue-i18n v11（intlify）—— 选定** | ★★★★★ | Vue 事实标准（MIT，约 390 万周下载）。Composition API + `globalInjection` 让模板直接用 `$t()`；纯 TS 目录模块可类型检查；当前目录规模无需额外构建插件 |
| i18next + i18next-vue | ★★★☆☆ | 生态最强，但 Vue 绑定是侧门（约 9.6 万周下载）；双层架构在此过度 |
| LinguiJS v6 | ★★☆☆☆ | ICU + 编译期抽取，但**无官方 Vue 运行时** |
| Paraglide JS 2（inlang） | ★★★★☆ | 最现代（编译消息可摇树、全类型安全）；生态较年轻，`setLocale` 默认整页刷新 —— 目录体积成为瓶颈时再评估 |
| FormatJS / vue-intl | ★★☆☆☆ | 完整 ICU/CLDR，但 Vue 绑定约 5k 周下载，ICU `}}` 与 Vue 模板冲突 |
| typesafe-i18n | ★★★☆☆ | 运行时极小、全类型；仓库已转移、维护缓慢；自有消息格式 |

选型逻辑：错误目录（`services/messages/`）*本来就是*按 code 索引的 per-locale 注册表——vue-i18n 只是把同一思想用于 UI 骨架，且生态与招聘池最大。

### 1.3 语言矩阵

| 层级 | 语言 | 状态 |
|---|---|---|
| 1（已交付） | `en`（默认）、`zh-CN` | 完整目录（user 应用 188 叶键、admin 306）、切换器、测试 —— 与文档站约定（`defaultLocale: 'en'` + `zh-CN` 镜像）一致 |
| 2（路线图，按需） | `zh-TW`、`ja`、`de`、`es`、`fr`、`pt-BR`、`ru` | 同类开源 IdP（Keycloak 翻译最全的语言集合）；社区有需求时按[工作流](#贡献者工作流)增补 |

## 2. 影响面清单

### 2.1 应用与路由

**`frontends/user`（用户门户）**—— 15 条路由：

| 路由 | 视图 | 用途 |
|---|---|---|
| `/login` | `pages/auth/LoginPage.vue` | 登录 + MFA、社交登录 |
| `/register` | `pages/auth/RegisterPage.vue` | 注册 |
| `/forgot-password` | `pages/auth/ForgotPasswordPage.vue` | 申请重置 |
| `/reset-password` | `pages/auth/ResetPasswordPage.vue` | 消费重置令牌 |
| `/verify-email` | `pages/auth/VerifyEmailPage.vue` | 邮箱验证结果 |
| `/callback` | `pages/oauth/CallbackPage.vue` | 授权码交换；协议错误码经共享目录映射 |
| `/callback/{github,google,wechat}` | `pages/oauth/{GitHub,Social}CallbackPage.vue` | 社交登录/绑定回调 |
| `/consent` | `pages/oauth/ConsentPage.vue` | OAuth 同意页 + scope 说明 |
| `/`、`/profile`、`/security`、`/authorized-apps` | `pages/account/*.vue` | 账户区（位于 `AppLayout` 内） |
| — | `layouts/AppLayout.vue`、`layouts/AuthLayout.vue` | 导航、用户菜单、主题切换、页脚 |

**`frontends/admin`（管理控制台）**—— 12 条路由，位于 `components/layout/AdminLayout.vue` 之下：`LoginPage`、仪表盘、应用（列表 + 详情页签）、用户（列表 + 详情）、角色、作用域、审计日志、令牌、设备审批、系统设置。

### 2.2 抽取前文案的存放位置

1. **模板字面量** —— 两应用硬编码英文，另有 admin 四处中文混排（授权类型描述 ×2 文件、一条校验消息、一段 Client Credentials scope 说明）。
2. **脚本常量** —— 导航项、scope 说明映射、授权类型选项数组、内联校验消息。
3. **错误目录** —— `services/messages/zh-CN.ts`（约 48 个码，含保留键 `__unknown__`/`__network__`、RFC 6749/7009/8628/6750 + OIDC 码），此前唯一的中文界面，现为双语。
4. **ARIA 标签 / 属性** —— 主题切换标签、告警关闭、弹窗关闭；对辅助技术可见，均已翻译。

### 2.3 约束本工作的跨应用不变量

- `components/ui/*.vue` 两应用**字节一致**，由 CI 的 `scripts/check-ui-sync.mjs` 强制 —— 共享组件（含 `LocaleSwitcher.vue`）必须两侧同步修改。组件模板只用 `$t()`（不引脚本依赖），同步因此天然成立。
- `services/errorAdapter.ts` + `services/messages/` 为镜像文件（权威源：user 应用）。镜像一致性由 **`scripts/check-ui-sync.mjs` 的错误目录检查**强制：user 目录中的每一条都必须在 admin 目录的对应语言中以完全相同的字面量存在（admin 可额外携带仅用于管理端的错误码）。前端测试无法跨应用检查这一点，因为测试文件禁止跨应用导入——`crossAppConsistency.property.test.ts` 只验证权威源自身是确定性的。镜像文件保持**零依赖**（不引 vue-i18n、不碰 DOM）：当前语言经由 `services/locale.ts` 纯模块传递，由 i18n 引导层推送。
- Property 13（`messageCatalog.property.test.ts`）要求**每个后端 Error_Code 与协议码**都有干净、非空的条目 —— 覆盖**每个已登记语言**。
- e2e 套件固定 `en-US` 运行（`playwright.config.ts` 的 `use.locale`）；中文路径由每应用专属 `i18n.spec.ts` 覆盖。

### 2.4 范围外（非目标）

- 后端响应消息（前端本地映射错误**码**；`/callback` 将协议 `error` 码走目录映射，仅在服务端返回 `error_description` 时作为次要详情行原样展示 —— 该字符串属后端）。
- 文档站（已由 Docusaurus 双语言化）。
- `<title>`（品牌名："Fulla" / "Fulla Admin"）、数字/日期格式化（有需要时用 `Intl`）。

## 3. 架构

```
src/i18n/
  index.ts     # createI18n(legacy:false, globalInjection:true)、检测、setLocale()、initI18n()
  en.ts        # UI 目录（命名空间：common/ui/nav[/auth/oauth/account | admin…])
  zh-CN.ts     # 镜像目录
  i18nKeys.test.ts  # 键奇偶校验 + 调用点键存在性（机械化门禁）
src/services/
  locale.ts    # 零依赖的当前语言状态（node 安全；单测友好）
  messages/    # 错误目录：en.ts + zh-CN.ts；getErrorMessage(code) 默认跟随当前 UI 语言
```

- **检测与持久化**（沿用 `fulla-theme` 模式）：`localStorage['fulla-locale']` → `navigator.languages` 最佳匹配（`zh*` → `zh-CN`，否则 `en`）→ `en`。`initI18n()` 在 `app.mount` 前**同步**执行 —— 无语言闪烁 —— `index.html` 内联脚本为首绘前预同步 `<html lang>`（读屏器正确性）。
- **一个切换器驱动一切（含一项已文档化的局限）**：`setLocale()` 更新 composer 语言、推送 `services/locale.ts` 状态（`normalizeError`/`getErrorMessage` 随之跟随）、持久化并同步 `document.documentElement.lang`。局限：错误消息**在错误触发时一次性解析**——错误状态存的是已解析字符串（`normalizeError(e).message`），切换语言后不会重译屏上已有文本，只有新触发的错误跟随新语言。该行为由 `i18n.spec.ts` 显式锁定。完全响应式变体（存 `code`、渲染时解析、响应式语言状态）是代价合理时的已知后续选项。
- **用法**：模板 `$t('auth.login.title')`（字节同步的 `components/ui` 同样适用）；`<script setup>` 中 `const { t } = useI18n()` —— **不带选项**（全局作用域）；纯 `.ts` 模块用 `i18n.global.t(...)`。`legacy: false` 下 `i18n.global.locale` 是 ref —— 脚本里读 `.value`，模板自动解包。响应式常量（导航项、选项数组）用 `computed` 包装。
- **错误**：`getErrorMessage(code)` 默认当前 UI 语言；`DEFAULT_LOCALE = 'en'` 仅作为未登记语言的回退表。
- **字体**：两应用都加载 Noto Sans SC，中文不会回退到系统字体。
- **无构建插件**：目录是 `createI18n` 直接导入的 TS 模块；当前目录规模下运行时消息编译（含编译器约 14 KB brotli）可接受。包体积预算吃紧时再评估 `@intlify/unplugin-vue-i18n`。

## 4. 落地方案与验收（已执行）

| 阶段 | 工作 | 验收标准 | 结果 |
|---|---|---|---|
| 1. 基础设施 | 两应用引入 `vue-i18n@11`；`src/i18n/` + `services/locale.ts`；`services/messages/en.ts` + 注册表改造；挂载 `LocaleSwitcher.vue`（user：顶栏 + 认证页脚；admin：顶栏 + 登录卡片）；admin 补中文字体 | build ×2 绿；切换持久化 + 同步 `<html lang>`；`check-ui-sync.mjs` 绿 | ✅ |
| 2. 文案抽取 | 全部视图/布局/组件翻译（en 逐字保留，admin 四处混排修复）；常量 → `computed` | tsc + build + lint ×2 绿；CJK 残留审计干净；键奇偶校验绿 | ✅ 188 + 306 叶键 |
| 3. 测试 | Property 13 → 全部语言；errorAdapter property → en 默认 + zh 显式；每应用 `i18nKeys.test.ts`；e2e zh→en；`i18n.spec.ts` ×2；钉死 `use.locale: 'en-US'` | unit + e2e + lint ×2 绿 | ✅ user 单测 34 / e2e 118；admin 单测 4 / e2e 185 + 6 环境跳过 |
| 4. 文档 | 本指南 + ADR-0013，en + zh-CN 镜像、侧边栏 | docs 构建绿；双写完整 | ✅ |
| 5. 全量验收 | 两应用全量前端套件 | lint、build、unit/property、e2e ×2 绿，无新增跳过 | ✅ |

**总体验收（语义）**：默认英文会话在所有路由上看不到前端自有中文字符串，简体中文会话看不到前端自有英文字符串 —— 由 CJK 审计（`grep -rn "[一-鿿]" src --include=*.vue --include=*.ts | grep -v "src/i18n/" | grep -v "src/services/messages/"` 无输出）与 `i18n.spec.ts` 机械化强制。具名例外：后端提供的字符串（如 `error_description` 详情行、成功 `message` 字段）、语言菜单里的原生语言名，以及字节同步切换器内部的 `'中文'` 短标签。

## 5. 贡献者工作流

**新增文案**：选/扩命名空间（如 `auth.login.title`），同 PR 在 **`en.ts` 与 `zh-CN.ts`** 双侧加键，调用点用 `$t()`/`t()`。`i18nKeys.test.ts` 门禁会在 en/zh 键漂移、调用点未知键、两应用 `ui.*` 分歧时失败。插值用命名参数（`{name}` 两侧都出现）；禁止拼接翻译片段。

**错误目录写作约束（Property 13 绊线 —— 永不放松匹配模式）**：消息文本不得含 `{{`、`}}`、`${`、`%s`、`%d`，不得含英文单词 *exception*、*traceback*、*stack trace*（单花括号 `{name}` 插值安全）。校验对**每个**已登记语言生效。

**新增语言**（二级）：创建 `src/i18n/<locale>.ts`（从 `en.ts` 翻译；机翻辅助**必须人工评审** —— 这是 MT 唯一允许的用途），在 `SUPPORTED_LOCALES` + `createI18n` + 切换器标签注册，添加镜像全部码的 `services/messages/<locale>.ts`，扩展奇偶校验测试的语言列表，并确认 CJK 字体覆盖。Property 13 + 奇偶校验会自动强制完整性。

**键约定**：命名空间 = 区域（`common`、`ui`、`nav`，user 另有 `auth`/`oauth`/`account`，admin 为 `admin.<page>`）；`ui.*` 键（供字节同步的 `components/ui/*` 使用）必须在**两个**应用解析一致 —— 由 user 应用的 `i18nKeys.test.ts` 跨应用校验强制。
