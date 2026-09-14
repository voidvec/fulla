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
