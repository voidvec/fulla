---
sidebar_position: 5
---

# 令牌生命周期

fulla 中令牌如何签发、存活与消亡：三类令牌、真正落库的是什么、刷新轮换如何
检测窃取、以及控制生命周期的每个开关。存储 schema 见
[数据与持久化](../architecture/data-persistence.md)；HTTP 契约见
[API 参考](api-reference.md)与 [ADR-0004](../adr/ADR-0004.md)。

## 1. 三类令牌，三种形态

| 令牌 | 形态 | 校验方式 | 默认生命周期 |
|---|---|---|---|
| Access token | **不透明随机串**（`generateSecureToken`） | 服务端状态（introspection / userinfo）——不是自包含 JWT | 3600 秒（`access_token_ttl`） |
| Refresh token | 不透明随机串 + **家族 id** | 服务端状态；每次使用即轮换 | 30 天（可配置） |
| id_token | **RS256 JWT**，经 JWKS 签名（`kid` 发布于 `/.well-known/jwks.json`） | 客户端侧验签（标准 OIDC） | 与 access token 相同；仅 `openid` scope 时签发 |

设计理由（[ADR-0004](../adr/ADR-0004.md)）：不透明 access token 的权威有效性
由服务端状态决定。Redis 缓存与副本有各自的收敛窗口（§7）；有状态撤销不代表
每个副本都会拒绝下一次请求。JWT 能力保留给协议要求它的 OIDC `id_token`。

一个常见混淆：令牌响应中携带的 **roles** 属于 JSON 信封
（`"roles": [...]`）与 id_token claims——不透明 access token 本身不携带任何
需要解码的内容。

## 2. 签发路径

所有授权类型汇合到 `TokenService`（libs/oauth2）：

| 授权类型 | 签发 | 说明 |
|---|---|---|
| `authorization_code`（+ PKCE，PUBLIC 客户端强制） | access + refresh（含 `openid` 时加 id_token） | 授权码单次使用、原子消费（见 §5） |
| `refresh_token` | 新 access + **新** refresh | 旧 refresh 被撤销；家族 id 继承（§4） |
| `client_credentials` | 仅 access（M2M，无用户） | 仅 CONFIDENTIAL 客户端；组织所属应用的令牌还保存 `org_id`（v1.4.0），可用时通过 introspection 暴露，subject 为 `client:<client_id>` |
| `device_code` | 用户批准后签发 access + refresh | 按 RFC 8628 轮询 |

每个响应中的 `expires_in` 宣告的是**配置的** access-token 生命周期——不是
硬编码 3600（RFC 6749 §5.1）。

## 3. 落库的是什么（以及不落库什么）

- access 与 refresh token 仅以 **SHA-256 哈希**持久化（写入仓储前经
  `hashToken()`）——拖库拿不到可用凭据（[ADR-0004](../adr/ADR-0004.md)）。
- refresh token 行额外存储 `token_family` id、关联的 access-token 哈希、
  `revoked`/`revoked_at`/`revoked_by`。
- 过期行由 `OAuth2CleanupService`（`cleanup_interval_seconds`，默认 3600 秒）
  清理——Postgres 定期 DELETE、Redis 历史上靠 TTL、memory 定期扫描。

## 4. 刷新轮换、重用检测与家族撤销

每次成功刷新签发**新的** refresh token，并继承同一 `token_family`（V008）。
生产 PostgreSQL 路径检测到保留的已撤销 RT 被重用时，会触发家族撤销；攻击者
与合法用户都可能需要重新认证。过期或记录不存在本身不等于重放。开发用 Memory
后端通过进程内 CAS 阻止重复使用，但其旧读取路径隐藏 revoked 记录，不具有与
PostgreSQL 等价的保留记录重放处置。威胁模型见
[安全架构 §7](../architecture/security-architecture.md)。

刷新没有宽限期，也不保证重试幂等。当前实现把旧 RT 消费与替代令牌对持久化分为
两次仓储调用；保存失败可能已经消费旧 RT，响应丢失也可能掩盖已经落库的新令牌对。
网络错误不能证明回滚，也不代表可以安全重用旧 RT。刷新失败后停止重试循环，重新
认证；不同标签页或进程应协调刷新。

**尚未发布的加固：**[PR #321](https://github.com/voidvec/fulla/pull/321) 引入原子
轮换、可区分结果的仓储接口与更强的缓存期限；升级到包含它的版本时才适用其自定义
仓储迁移要求。不能把这些保证套用到更早的构建。

## 5. 授权码单次使用

`consumeAuthCode` 在各后端均原子：Postgres 用
`UPDATE ... WHERE consumed = false ... RETURNING`、Redis 历史上用 Lua 脚本、
memory 用互斥锁——竞态中被重放的窃取授权码必然失败。该契约由
`GrantRepositoryContractTest`（`ctest -L Contract`）跨三实现强制守护。

## 6. 撤销面

| 面 | 粒度 |
|---|---|
| `POST /oauth2/revoke`（RFC 7009） | 被提交的令牌 |
| 管理 API `DELETE /api/admin/tokens/{prefix}` | 按令牌前缀 |
| 管理令牌面 | 按**客户端**或按**用户**撤销——清掉该主体的全部令牌 |

权威存储成功撤销后，读取到该状态的调用会拒绝令牌；既有缓存快照或滞后副本可能
延迟观察到结果（§7）。必须先检查 introspection 的 `active`，为 false 时忽略其余
元数据。userinfo 观察到令牌已撤销时返回 401。

登录资格、当前授权和令牌有效性是不同判断：

| 管理操作 | 当前行为与边界 |
|---|---|
| 禁用用户 | 设置登录锁并失效用户读取缓存，不自动撤销全部已签发 AT/RT；显式令牌撤销是另一项操作。 |
| 移除用户的角色绑定 | 受角色保护的 API 解析角色/权限数据；不会重写 token scopes，角色缓存失效与存活时间仍需考虑。 |
| 修改角色的权限定义 | 使用独立、无 TTL 的进程内权限缓存；需在每个副本失效或重启。用户角色 TTL 不能限定这种变更的生效时间。 |
| 移除组织成员或授权 | 组织 gate 和 claim 解析检查各自当前状态；已存 `org_id` 是历史上下文，不是实时会员授权，也不会自动撤销已签发令牌。 |
| 停用开放平台客户端 | 客户端校验与 introspection 的 owner 状态覆盖层各有缓存；并不等于对所有 API 通用的令牌撤销。 |

组织决定原子性及响应完成加固见 [PR #322](https://github.com/voidvec/fulla/pull/322)，
它们不会改变上述已签发令牌的撤销边界。

## 7. 与缓存的联动

启用 Redis L2（`cache.enabled`）后，token/client 读取先查 `fulla:cache:*` 再回源
PostgreSQL。直接撤销 AT 会尝试写负标记，并立即及延迟删除 access/introspection
条目。第二次 DEL 默认延迟 200ms（`cache.invalidation_double_delete_delay_ms`，
钳位到 50–2000ms），可减少回填竞态，但不能证明每次失效都成功。family/RT 撤销与
pair 写入直接委托存储，并不是所有路径都清理正缓存。

| 缓存 | 默认条目存活时间 |
|---|---|
| Access/introspection | `cache.ttl_seconds.access_token_max`：60 秒，并受 token 到期时间限制 |
| Client | `cache.ttl_seconds.client`：300 秒 |
| 用户角色 / profile | `cache.ttl_seconds.user_roles`：120 秒 / `user_profile`：300 秒 |
| Introspection owner 状态 | 进程内 memo 60 秒，与 client 缓存独立 |

这些是条目存活时间，不是从撤销时刻开始计算的已证明期限。当前 token 回填在后端
读取完成后计算相对 TTL；在途读取、排队写入和副本滞后都可能让观察到变更的时间
超过一次配置 TTL。第二次 DEL 同样只是尽力执行，不能由这些默认值推导出跨副本
绝对撤销 SLO。PR #321 提议的 token 缓存绝对预算也不能推广到其他缓存。

当前 Redis token payload 不保留 `org_id`，缓存命中时可能省略该字段；缺少它不证明
是个人上下文，资源服务器仍须另行建立所需的组织授权。失效失败通过
`fulla_cache_invalidation_failures_total` 可观测（见[可观测性](../operate/observability.md)）。
存储路径详见[数据与持久化](../architecture/data-persistence.md)。

## 8. 配置参考

| 键 | 默认 | 含义 |
|---|---|---|
| `access_token_ttl` | 3600 | access-token 生命周期（秒）；在 `expires_in` 中宣告 |
| refresh-token TTL | 30 天 | refresh-token 生命周期 |
| `cleanup_interval_seconds` | 3600 | `OAuth2CleanupService` 清理过期行的周期 |
| `cache.enabled` / `cache.ttl_seconds` | false / — | token/client 读的 L2 缓存（[配置指南](../operate/configuration-guide.md)） |
