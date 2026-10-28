# 配置指南

## 1. 环境变量注入

应用支持用环境变量覆盖关键配置项。这在 Docker/Kubernetes 环境下尤为重要——
敏感信息不应硬编码在 `config.json` 中。

### 支持的环境变量

| 变量名 | 说明 | 覆盖的配置路径 | 示例 |
|---|---|---|---|
| `FULLA_DB_HOST` | 数据库主机名 | `db_clients[0].host` | `postgres` |
| `FULLA_DB_NAME` | 数据库名 | `db_clients[0].dbname` | `fulla_db` |
| `FULLA_DB_PASSWORD` | 数据库密码 | `db_clients[0].passwd` | `secret` |
| `FULLA_REDIS_HOST` | Redis 主机名 | `redis_clients[0].host` | `redis` |
| `FULLA_REDIS_PASSWORD` | Redis 密码 | `redis_clients[0].passwd` | `secret` |
| `FULLA_PORTAL_CLIENT_SECRET` | 门户客户端密钥（旧名别名：`FULLA_VUE_CLIENT_SECRET`） | `plugins[OAuth2Plugin].config.clients.fulla-portal.secret` | `...` |

> 生产部署的完整环境变量清单（30+ 项）见[生产部署](deployment.md)的变量表；本表只列注入机制的六个核心项。

### 工作机制

1. **加载钩子**：启动时 `main.cc` 的 `loadConfiguration()` 先调用 `common::config::ConfigManager::load()`，再调用 `ConfigManager::validate()`。
2. **解析**：把基础 `config.json` 读入 `Json::Value` 对象。
3. **注入**：检查上述环境变量是否存在；存在则就地更新 `Json::Value` 中对应节点。
4. **加载**：Drogon 通过 `drogon::app().loadConfigJson(config)` 直接加载修改后的配置对象，磁盘上不产生临时文件。

### 验证

专用测试 `EnvInjectionVerify`（`EnvConfigTest.cc`）保证该逻辑正确。

## 2. Docker 部署

仓库内置 `docker-compose.yml` 编排全栈（详解见 [Docker 部署](docker-deployment.md)）。

### 服务栈

- **fulla-frontend**：Vue SPA + Nginx（构建自 `deploy/docker/Dockerfile` 的 `frontend-runtime`）。
- **fulla-admin**：管理后台前端（构建自 `frontends/admin/Dockerfile`）。
- **fulla-backend**：Drogon 后端（构建自 `deploy/docker/Dockerfile` 的 `backend-runtime`）。
- **fulla-postgres**：PostgreSQL 17（后端启动时经 `FULLA_AUTO_MIGRATE=true` 应用 `apps/server/migrations/` 下的 schema）。
- **fulla-redis**：带密码保护的 Redis 7。
- **fulla-prometheus**：指标采集。

### 快速开始

```bash
# 构建并启动（在仓库根目录执行）
docker compose -f deploy/docker/docker-compose.yml up -d --build

# 查看日志
docker compose -f deploy/docker/docker-compose.yml logs -f fulla-backend

# 停止
docker compose -f deploy/docker/docker-compose.yml down
```

### Docker 下的配置处理

`docker-compose.yml` 将 `apps/server/config/config.json` 只读挂载进容器；
`environment` 段注入环境变量（见 §1），运行时经 `ConfigManager::load()` +
环境注入覆盖文件默认值。

## 3. 存储后端选择

OAuth2 插件的 `config.storage_type` 决定持久化后端：

| `storage_type` | 状态 | 说明 |
|---|---|---|
| `postgres` | **支持（唯一生产后端）** | 完整令牌持久化、refresh token 轮换与重用检测。 |
| `redis` | **已弃用** | 历史上从未持久化 refresh token（`saveRefreshToken`/`getRefreshToken` 为空操作），轮换与重用检测静默失效。该模式仍可启动（兼容考虑，启动时打 ERROR 日志），但 `refresh_token` 授权会以 `unsupported_grant_type` 被拒绝。新部署不要使用。 |
| `memory` | 仅测试 | 面向单元/集成测试，不用于生产。 |

目标架构：**Postgres 作为存储层，前置一个在线 Redis L2 缓存**（键空间
`fulla:cache:*`，经 `config.json` 的 `cache` 块配置 `enabled` /
`ttl_seconds` / `invalidation_double_delete_delay_ms`；失效采用延迟双删，
见 `DelayedDoubleDelete`）。不存在独立 Redis 存储模式。

## 4. Issuer 配置

`config.metadata.issuer`（custom config）是服务器 issuer URL 的唯一事实源。
`OAuth2Plugin` 启动时读取一次，一致地用于：

- 签发 access token 时打上的 `iss` 声明（authorization_code / refresh_token / client_credentials / device_code 授权）；
- 内省响应的 `iss`（存储行未携带时以配置值回填）；
- 发现文档（`/.well-known/openid-configuration`、`/.well-known/oauth-authorization-server`）。

约束：

- 末尾斜杠会被自动规范化掉，不要依赖它。
- 未设置时默认 `http://localhost:5555`，此时打 `LOG_WARN`。
- 生产部署**必须**配置 `https://` issuer；非回环主机上使用明文 http issuer 会有启动告警。
- 内省 `iss` 与发现文档 `issuer` 保证逐字节一致（OIDC Discovery §3 要求）。

## 5. 客户端令牌端点认证方式（F-017）

每个客户端通过 `oauth2_clients.token_endpoint_auth_method` 列声明其在
`/oauth2/token`、`/oauth2/introspect`、`/oauth2/revoke` 的认证方式：

| 取值 | 语义 |
|---|---|
| `client_secret_basic` | 密钥**必须**走 `Authorization: Basic` 头；body 携带 `client_secret` 会被拒绝。 |
| `client_secret_post` | 密钥**必须**走 POST body；Basic 头会被拒绝。 |
| `none` | PUBLIC 客户端；携带任何 `client_secret` 都会被拒绝。 |
| NULL / 空 | 旧版宽松回退：接受 Basic 头，也接受 body 密钥（Basic→body 回退）。 |

注册/管理端创建时若省略该字段，按以下默认值落库：

- `PUBLIC` 客户端 → `none`（本就没有密钥）。
- `CONFIDENTIAL` 客户端 → `client_secret_basic`。

种子客户端均显式声明：`fulla-portal` 与 `fulla-admin-console` → `none`；
`backend-svc` → `client_secret_basic`。已有 NULL 值的客户端保持升级前的
行为，升级不破坏存量部署。

## 6. OIDC prompt / max_age / auth_time（F-022）

授权端点支持 OIDC Core §3.1.2.1 的 `prompt` 与 `max_age` 参数：

- **`prompt=none`**：禁止任何 UI。无会话 → 302 `error=login_required`；
  需要同意 → `error=consent_required`。错误会带着回显的 `state` 重定向回
  已验证的 `redirect_uri`。`none` 与其他值组合（如 `none login`）自相矛盾，
  直接返回 400。
- **`prompt=login`**：即使已有会话也强制重新认证。
- **`prompt=consent`**：即使既有同意已覆盖所请求的 scope，也强制显示同意页。
- **`max_age=<秒>`**：若会话的 `auth_time`（登录 / MFA 验证时设置）早于
  `max_age`，强制重新认证。

`auth_time` 与 `amr` 随授权码持久化，并在换票时打进 id_token：`auth_time`
（大于 0 时）、`amr`（设置时为 JSON 数组）、`acr`（`1` = 仅密码，`2` = MFA）。
发现文档宣告 `prompt_values_supported`、`acr_values_supported` 与相关 claims。

## 7. RP-Initiated Logout（F-027）与会话失效（F-028）

`/oauth2/end_session`（GET + POST）终结服务端会话。要在登出后重定向，
客户端须提供 `post_logout_redirect_uri`，且它**必须**是该客户端已注册的
redirect URI 之一；客户端由 `id_token_hint` 的 `aud` 声明识别。hint 的
签名**会**被验证（RS256 + kid + iss/exp/sub 策略，issue #78），验证失败以
400 `AUTH_INVALID_ID_TOKEN_HINT` 拒绝。没有合法 hint + 已注册 URI 时请求
被 400 拒绝；成功时带 `state` 回显 302 重定向，未提供重定向 URI 时返回 200。

`POST /oauth2/logout`（既有的 API 登出）额外调用 `session()->clear()`
（F-028），因此服务端会话随 access token 撤销一并终结。

## 8. 认证失败限流（F-018）

token / introspect / revoke / device-code 轮询四个端点共享一个进程内
滑动窗口限流器，按 `(client_ip, client_id)` 分桶。滚动窗口
（默认 60s）内**失败**计数达 `max_failures`（默认 30）后，后续请求返回
**HTTP 429**，带 `Retry-After` 头与 OAuth2 风格的
`{error, error_description}` 响应体。只计**失败**；一次成功即清零，因此
正常负载（以及大量连续成功请求的集成测试套件）不会被限流。

经 `custom_config.auth.rate_limit` 配置（所有 `config*.json` 都显式携带默认值）：

```json
"custom_config": {
  "auth": {
    "require_pkce_for_public": true,
    "allow_http_redirect_uri": true,
    "rate_limit": {
      "max_failures": 30,
      "window_seconds": 60
    }
  }
}
```

两个键均可省略；`rate_limit` 对象缺失时使用内置默认（30 / 60）。限流器是
函数局部单例（`libs/common/include/fulla/common/utils/RateLimiter.h` 的
`RateLimiter::instance()`），四个受保护端点在同一进程内共享一份计数表。
这是最小化的暴力破解/令牌探测防护；多实例部署需要共享存储（Redis），
为后续工作。

## 9. JWKS 密钥轮换（F-029 —— 运维待办）

JWKS 端点（`/.well-known/jwks.json`）当前只服务**单个静态 `kid`**，在插件
启动时由配置的 JWK 物料初始化一次（`OAuth2Plugin::initAndStart()` →
`JwkManager::init()`）。**轮换尚未实现**：没有轮换周期、没有 kid 滚动窗口、
没有双密钥发布。当前密钥签发的令牌在其整个生命周期内有效；轮换密钥会使
前任密钥签发的所有存量令牌失效。

生产轮换已登记为运维待办。在此之前，将密钥泄露纳入威胁模型的运维者应
以新 JWK 物料重启服务器（并接受此前签发的全部令牌失效）。

## 12. 邮件模板（P1-2，#278）

三封生产邮件（邮箱验证、密码重置、组织邀请）改为从数据库模板渲染
（V040 `email_templates`：3 个 key × `en`/`zh-CN` 种子，每行含主题 +
HTML + 纯文本）。发送通道仍由环境变量驱动（`FULLA_SMTP_*` 选择 Console
或 SMTP）；模板在其上叠加本地化、可编辑与 multipart/alternative HTML
投递。

管理 API（admin 角色 + `roles:read`/`roles:write` scope）：
`GET /api/admin/email-templates`、`GET|PUT /api/admin/email-templates/{key}/{locale}`、
`POST /api/admin/email-templates/{key}/{locale}/test-send`。仅 Postgres——
memory 部署返回 404（无模板存储；实际发送时引擎回退到编译期内建模板）。

模板变量使用 `{{name}}` 占位符；引擎对每个替换值做 HTML 转义（组织名是
无过滤自由文本）。保存时内容策略拒绝 `<script>/<iframe>/<form>/<object>/
<embed>/<base>` 标签、`javascript:`/`data:`/`vbscript:` URL，以及任何
不在允许列表内的外链 `http(s)` href（见下方 `allowed_link_domains`），
返回 `EMAIL_TEMPLATE_CONTENT_REJECTED`（400）。

配置（custom_config）：

```json
"email": {
    "allowed_link_domains": ["portal.example.com"],
    "test_recipients": ["admin@example.com"],
    "test_send_quota_per_day": 10,
    "allow_org_logo": false
}
```

- `allowed_link_domains` —— 保存模板中字面 `http(s)` href 允许的主机
  （精确或子域匹配；条目接受裸主机或 `https://host/` 写法）。为空或缺省
  = 不允许任何外链；变量链接（`{{verify_link}}`、`{{reset_link}}`）由引擎
  供给，不受此列表影响。
- `test_recipients` —— 管理端 test-send 额外授权的收件地址（各管理员
  本人账号邮箱始终允许）。
- `test_send_quota_per_day` —— 每管理员滚动 24 小时 test-send 配额
  （默认 10；按最近 24 小时的 `email_test_sent` 审计行计数——刻意不是
  自然日）。
- `allow_org_logo` —— 邀请模板中组织品牌变量的占位开关；**邮件中的远程
  组织 logo 是收件人打点隐私面，默认保持关闭**。种子模板不引用组织
  logo 变量。

**链接信任锚（D8 红线）**：验证与重置链接的域名只来自全局 `frontend.url`
配置——组织可控的输入永远不会被拼进安全链接；且无论配置如何，验证/重置
邮件不携带组织品牌。

## 13. 审计日志导出与保留（P1-6，#283）

`GET /api/admin/logs/export`（admin 角色 + `audit:read` scope）以 CSV 附件
形式导出审计流水。过滤器与列表端点相同（`action`/`outcome`/`actor_id`），
另加**强制时间窗**（`from`/`to`，unix epoch 秒）——除非调用方以 `all=true`
显式选择全表；行数硬上限防止无界导出。单元格做了公式注入加固：以
`= + - @` 或制表符/CR 开头的单元格前置撇号（OWASP）——`user_agent`、`ip`
与 `details` 完全用户可控，导出是注入出口。每次导出都会写审计
（`audit_exported`）。

配置（custom_config）：

```json
"audit": {
    "retention_months": 0,
    "export_max_rows": 500000
}
```

- `retention_months` —— 审计保留窗口（默认 0 = 永久保留）。清理周期
  （与推进分区前瞻的同一周期）会 DROP 整个月都在窗口之前的
  `audit_logs_pYYYY_MM` 月分区。只有精确匹配该命名模式的分区才会成为
  候选；DEFAULT 分区永远不会被 DROP。被 DROP 历史的恢复方式 = 重新导入
  导出的 CSV。
- `export_max_rows` —— 导出行数上限（默认 50 万）。请求级 `limit` 参数只
  允许调低；超限导出返回 400 `AUDIT_EXPORT_TOO_LARGE` 并提示收窄窗口。

## 14. 品牌化消费链 + PKCE plain 移除（P1-1，#279）

**PKCE `plain` 现被无条件拒绝。** v1.5.0 作为单版本过渡发布的
`auth.allow_pkce_plain` 逃生阀已移除（过渡窗口已关闭）。配置键已删除：
authorize 处 `code_challenge_method=plain` 无论配置如何都返回
`invalid_request`；discovery 只公告 S256。仍依赖 plain 的部署必须在升级
v1.6.0 前把客户端迁移到 S256。

**品牌化消费。** 组织与客户端品牌字段现在到达用户侧界面：

- `GET /oauth2/authorize-context`（additive）：客户端 `logo_uri`，以及
  应用归属组织时的 `{org_name, logo_uri?, primary_color?}` 块。
- `GET /oauth2/consent/context`（additive）：客户端展示块、本地化的
  `scope_descriptions`（见下）、org 横幅上的品牌字段。品牌仅作展示、
  不受信——始终与所有者归因标签成对渲染。
- 同意屏的 logo 只以 `<img src>` 渲染（SVG 在 img 上下文不执行脚本；
  禁止以锚点包裹）。nginx 层（用户 + 管理 SPA）现在下发
  Content-Security-Policy（`img-src 'self' data: https:`）——此前两个
  SPA 完全没有 CSP。

**写路径与校验**（V041 增加 `oauth2_clients.logo_uri` 与
`oauth2_scopes.description_zh`）：管理端客户端创建/编辑、门户我的应用
创建/编辑接受 `logo_uri`；组织自助创建校验两个品牌字段。规则（刻意
不是 redirect 注册规则）：

- `logo_uri`：仅 https，回环主机豁免（开发用），禁 userinfo/fragment，
  ≤512 字符——不继承 `auth.allow_http_redirect_uri`。
- `primary_color`：`^#[0-9a-fA-F]{3,6}$`（原先无约束；列宽 VARCHAR(7)）。
- `description_zh`（作用域管理 CRUD，≤512 字符）：中文同意屏文案。
  consent context 按页面语言服务端下发；前端字典仅作缺失兜底。描述
  变更带前后 diff 写审计（`scope_updated`）。

## 15. RBAC 权限 enforcement（P1-8，#285）

委托管理已上线：三道门 RBAC 模型（ADR-0014 修订 1）——scope 分层（链①）、
IdentityService scope 检查（链②）、AuthorizationFilter 角色/权限门（门③）
——现在从 `role_permissions` 目录解析权限（V043 种子；V005 旧名原地标记
退役）。

- **rbac_rules 对象形式**（additive）：条目可以写
  `{"permissions": ["users:read"]}` 替代角色数组——调用者的
  角色→权限并集须覆盖每个列出的名字。具体规则先于
  `/api/admin/.* -> ["admin"]` 兜底匹配（加载时强制最具体优先）。
  发行配置演示了 helpdesk 形态：`/api/admin/users.*` 要求
  `users:read`。
- **分类与判定解耦**：哪些 scope 属 admin 级仍来自
  `oauth2_scopes.requires_admin_role`（本版继续读取的弃用列）；用户能否
  通过改为按权限判定（或字面 admin 超角色）。给角色授 scope 永不改变
  分类。
- **登录路径同样分层**：两步门户登录铸造的 code 只包含三层引擎判定
  通过的 scope（此前未过滤——委托控制台会拿到其用户无权限的 admin 级
  scope）。
- **角色权限管理**：`GET/PUT /api/admin/roles/{id}/permissions`
  （全量替换；名字必须全部存在；审计 `role_permissions_updated`；提交时
  单实例缓存失效——多实例漂移是已知局限，P3 共享存储解决）。
- **memory 模式**：编译期默认映射（admin → 全十项，user → 无）镜像
  V043 种子。
