# 刷新令牌与撤销生效边界

S03 安全加固保留严格单次刷新令牌和家族重用检测，调整轮换的持久化方式并明确缓存收敛边界。本文描述尚未发布的行为，以及自定义 C++ 仓储所需的迁移。

## 1. 轮换与失败结果

令牌端点先认证客户端，仓储再检查 RT 是否属于该客户端、尚未到期且未被使用。错误客户端不能消费合法 RT，也不能通过提交已使用 RT 撤销其他客户端的家族。

subject 规范化、claim 解析及随机令牌生成均在消费之前完成。Memory 在一个 mutex 内轮换；Postgres 持有事务级家族 advisory lock，在同一事务中消费旧 RT、插入两个新令牌，只有确认 commit 后才报告成功。轮换与家族撤销使用相同锁域。

| 故障或事件 | 持久化状态 | 客户端结果 |
|---|---|---|
| 消费前查询或存储失败 | 旧 RT 不变，无新 pair | `server_error`，不返回令牌 |
| 随机源或 subject 规范化失败 | 旧 RT 不变，无新 pair | 错误，不返回令牌 |
| client 不匹配或未使用 RT 的 `now >= expires_at` | 不消费、不撤销家族 | `invalid_grant` |
| AT/RT 任一插入失败或确定性提交拒绝 | 旧消费回滚，无新 pair | `server_error`，不返回令牌 |
| COMMIT 确认丢失 | 完整旧状态或完整轮换状态，调用方无法确定 | `server_error`，不返回未经确认的令牌 |
| 提交成功但 HTTP 响应丢失 | 旧 RT 已使用，完整新 pair 存在 | 可能需要重新登录 |
| 正确客户端重放仍保留的已使用 RT | 撤销整个家族，包含已提交的新 pair | `invalid_grant`；撤销存储失败则为 `server_error` |

提交结果不确定与确定回滚不同。不能因网络错误推断旧 RT 仍可使用。重试已使用 RT 可能撤销整个家族，包括响应丢失的那组令牌。不增加宽限期或服务端重试幂等；单个客户端实例合并刷新请求不能协调其他标签页或进程。

两个请求使用同一旧 RT 时最多一个轮换成功。若另一个请求随后检测重用，已成功返回的 pair 也会被撤销。到期本身不能证明重用；仍保留的已使用记录即使过期也继续作为重放证据，保持既有 Postgres 政策，绝不允许再次兑换。

当关联 AT 仍有效时，清理任务保留已到期 RT 的关联元数据，覆盖 RT 期限短于 AT 的配置；这只保留家族撤销的可达性，不延长 RT 使用期限。AT 到期、被撤销或不存在后，后续清理即可回收元数据，不新增永久 tombstone 或 schema 变更。

## 2. C++ 仓储迁移

自定义 `ITokenRepository` 必须实现以下加性方法：

- `inspectRefreshToken`：权威读取，区分 Ready、NotFound、AlreadyUsed、ClientMismatch、Expired 和 StorageError；仅向正确绑定的客户端返回已使用令牌元数据。
- `rotateRefreshToken`：在临界区重新校验权威旧记录和期限，原子消费并保存新 pair，仅确认持久化后返回 Rotated。
- `revokeTokenFamilyChecked`：与轮换共用家族锁，原子撤销两类令牌并报告确认的成功或失败。

原 CRUD/CAS 方法保留。新方法默认失败关闭，所以旧实现仍能编译，但迁移前刷新请求会返回 `server_error`。这是源码兼容，不是行为兼容，也不提供二进制 ABI 承诺。不能把旧接口的所有 nullopt 判成重用，或将顺序写入宣称为原子操作。

Drogon 宿主的 introspection 归属判断必须使用 lookupOwnerStatusChecked。OwnerStatusResult 区分 Active、成功确认无归属的 Unmanaged、Suspended（包含未知行状态）及 Unavailable，仅前两种可继续 introspection。旧 optional lookupOwnerStatus 包装为展示和源码兼容保留，仍丢失错误与不存在的区别，不可用于授权。

内置 Memory/Postgres 仓储与 Redis 缓存装饰器实现了新路径。已弃用的独立 Redis 存储继续拒绝刷新 grant。按[版本政策](../contribute/versioning-and-release.md)，下一次 MINOR 发布必须显著标记此安全行为收紧与迁移要求。

## 3. 令牌缓存收敛

Postgres 的 AT 校验与 introspection 从主库读取，缓存回退也使用主库，避免由落后 reader 重新填入安全缓存。未命中或故障时主库读取量会增加，应据此配置主库容量。

令牌 L2 使用 `cache.ttl_seconds.access_token_max`（默认 60 秒），并受令牌到期时间限制。过期预算从异步查询前起算；Redis EXAT 保留绝对期限，迟到的数据库回调或 Redis 命令不能重新获得完整 TTL。Lua 在填充时原子检查撤销标记与期限；introspection 同时检查 access key，RT introspection 不进入缓存。两种缓存载荷均保留 org_id，旧版无版本标记的载荷按未命中处理。

直接撤销 AT 会尝试写负标记并执行立即和延迟 DEL。家族撤销不逐一清理正缓存，依照既有 TTL 政策收敛。失效失败时，已缓存的撤销前令牌可能有效至其绝对缓存期限与 token 到期时间中较早者。在应用与 Redis 时钟同步、主库权威读取的前提下，撤销前快照不能将期限推迟到撤销后一个配置预算之外。运维需要计入时钟偏差，TTL 不代表所有副本的下一请求必然拒绝。

缓存读取有一个覆盖 Redis 查询、后端查询和受保护填充的五秒本地完成期限。首次 Redis 错误回退权威仓储；受保护填充错误或本地超时则拒绝读取，不返回未经确认的旧快照。迟到回调不能再次完成请求，业务回调会被释放。宿主必须运行承载这些定时器的 Drogon event loop。超时及错误计入 `fulla_cache_total{repo="token",outcome="error"}`；失败 DEL 继续计入 `fulla_cache_invalidation_failures_total{kind="token"}`，负标记 SET 失败也可观测。

## 4. 权限变化是不同的操作

令牌有效性、登录资格和当前授权是不同检查；不会将现有政策改成统一的即时撤销：

| 变化 | API 与刷新行为 | userinfo/introspection 和缓存边界 |
|---|---|---|
| 禁用用户 | 设置登录锁，不自动撤销全部已签发 AT/RT | profile 缓存失效不等于令牌撤销；终止既有访问需显式撤销相关令牌 |
| 撤销用户角色 | 角色保护 API 解析角色和权限；已签发 scope 不被改写 | 用户角色缓存默认 120 秒，写入钩子失效已知 subject 形式；userinfo claim 不能替代 API 授权检查 |
| 移除组织成员 | 组织感知 gate 与 claim 解析检查成员关系；存储 org_id 仍为历史绑定 | 失去成员关系后，新 userinfo/刷新 ID token 不再带 org_ctx；introspection org_id 不证明当前成员资格 |
| 停用开放平台客户端 | 新 client lookup 拒绝明确已停用的 owner；该读取不统一撤销既有 AT | introspection 使用类型化 owner-status overlay；查询错误或超时仅返回 active:false。client 与 owner-status 缓存仍独立，其他 API 不因此新增该 overlay |

owner-status memo 采用从读起点算起的 60 秒预算和五秒完成期限，仅缓存成功 Found/NoRow，错误、超时及其迟到回调均不能重新填充。失效仍可能与在途成功读取竞争，但不会重启其原预算。其他已有缓存保留各自政策：client L2 默认 300 秒，用户角色 120 秒，用户资料 300 秒。这些是条目存活时间，不是已证明的“提交到拒绝”上界，还需计入异步填充和后端读取耗时。不能把上文 token 缓存的绝对预算保证推广到这些缓存。若后端读取或副本没有有界的新鲜度与延迟保证，仅配置 TTL 无法证明更严格的全局撤销 SLO。

表中的角色撤销指移除用户的角色绑定。修改现有角色所授予的权限，另有进程内 RolePermissionResolver 缓存且没有 TTL。其既有政策要求对每个副本主动失效或重启；用户角色 TTL 不能约束权限定义变更。本项不新增跨进程失效协议。

若要立即终止既有会话，必须执行并验证显式令牌撤销；没有这一步，不能把禁用登录或修改角色宣传成撤销现存令牌。变更提交时已在授权中的请求，也不同于提交后才开始的新请求。

## 5. 验证与运维限制

回归覆盖 Memory/Postgres 的绑定校验、任一 pair 插入冲突、延迟提交拒绝、单次消费并发、等待家族锁时到期，以及在 CAS 到 INSERT 之间暂停轮换后并发家族撤销。域测试覆盖查询/轮换故障、随机源失败、提交结果不确定、响应丢失后的重放语义和仅实现旧 SDK 接口的仓储。owner-status 测试分别验证状态映射、无后端及真实 Postgres 锁等待超时，不将前置客户端认证失败冒充 overlay 验收。缓存测试用独立节点对象共享真实 Redis，配合命令故障与迟到快照，验证共享 L2 行为，不冒称跨主机部署测试。

应分别保留确定性数据库拒绝、数据库提交结果不确定和 HTTP 响应丢失的证据。模拟确认丢失测试不能称为真实网络分区实验。本项不修改 token 格式、默认令牌期限、consent 政策或组织权限含义，不执行生产部署。
