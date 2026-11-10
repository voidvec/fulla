---
sidebar_position: 7
---

# 多租户（Organizations）

fulla 的多租户目前是一个**组织层**：组织归组用户与客户端、承载品牌字段、
锚定 v1.4.0 的租户语义（组织上下文授权、组织授权（organization consent）、owner 继任），
并经管理 API 与自助门户流程管理。本页如实记录现状有什么、**没有**什么、以及如何在不过度
假设隔离的前提下使用它。

> **先读这段**：组织是元数据与归属归组，不是硬隔离边界。授权由 RBAC +
scope 强制（见 [RBAC 指南](rbac-guide.md)）；今天属于某组织的主体并不会
自动与其他组织的数据隔离。

## 1. 模型（V017 + V034/V036–V038 + V042）

核心 `organizations` 表来自 V017；组织层如今又横跨五条迁移——
`organization_members` + `organization_invitations`（V034）、
`organization_consents`（V036；同一迁移还新增 `organizations.require_mfa`、
授权码/令牌上的组织列、`audit_logs.org_id`，并播种 `org` scope）、
`organization_succession_nominations`（V037）、`organization_consent_requests`（V038），
以及 `organizations.deleted_at` 软删除标记（V042）：

```sql
organizations (
    id              SERIAL PRIMARY KEY,
    slug            VARCHAR(50) UNIQUE,   -- 3–50 字符，小写
    name            VARCHAR(200),
    logo_uri        VARCHAR(512),         -- 品牌
    primary_color   VARCHAR(7),           -- 品牌
    issuer_override VARCHAR(512),         -- 已存储；见 §4 路线图
    require_mfa     BOOLEAN DEFAULT FALSE,-- V036；由组织上下文门强制
    created_at / updated_at
)
```

两个可空外键把实体挂到组织上：

| 列 | 所在表 | 语义 |
|---|---|---|
| `org_id` | `users` | V017 的单组织锚点，已废弃。**v1.4.0 起只读**（admin API 写入面已移除）；`NULL` = 未设置。计划 v2.0 物理删除。 |
| （已删除） | `oauth2_clients` | V017 的 `org_id` 列从未被代码读写，**V036 已 DROP**。客户端归属在 `oauth2_client_owners.org_id`（v1.4.0）。 |

v1.4.0 起组织归属的唯一权威锚点是 M:N 成员表（`organization_members`）与
`oauth2_client_owners.org_id`；`users.org_id` 只是为迁移保留的只读遗留列。

## 2. 管理 API 面

所有路由要求 admin scope 令牌（`AuthorizationFilter`；`impliedBy: admin`）——
[API 参考](api-reference.md)客户端管理一节：

| 方法与路径 | 用途 |
|---|---|
| `GET /api/admin/organizations` | 列出组织（id、slug、name、品牌字段） |
| `POST /api/admin/organizations` | 创建（slug：3–50 个小写字符，唯一） |
| `GET /api/admin/organizations/{slug}` | 查询单个 |
| `PATCH /api/admin/organizations/{slug}` | 更新 name / `logo_uri` / `primary_color`（v1.6.0，P1-4 #281；slug 不可变；品牌字段按 P1-1 校验规则；审计 `org_updated`） |
| `DELETE /api/admin/organizations/{slug}` | 软删除 + 封闭级联（v1.6.0，P1-4 #281；V042 `deleted_at` + `#del-<id>` slug 占位）：作废待处理邀请与继任提名、撤销组织授权、挂起所属客户端（其令牌 introspect 为 inactive）、解除成员关系——单事务；审计 `org_deleted` |
| `GET /api/admin/organizations/{slug}/members` | 只读成员名册（user_id、username、role、joined_at）（v1.6.0，#281） |
| `DELETE /api/admin/organizations/{slug}/members/{userId}` | 移除成员关系——移除最后一个 owner 被拒（409，需先转移所有权）；审计 `org_member_removed`（v1.6.0，#281） |
| `POST /api/admin/organizations/{slug}/transfer-ownership` | 把 owner 席位转给另一位在世用户（解软删 owner 死锁；旧 owner 降级为 admin） |

另：

- 管理用户 API（`POST/PUT /api/admin/users`）**不再接受 `org_id`**（v1.4.0
  组织锚点收敛）：请求体携带该键一律 400，无论取值。组织成员关系经组织
  成员 API（`organization_members`）管理；`users.org_id` 列在 v2.0 物理删除
  前保持可读。

示例：

```bash
# 创建组织（令牌：admin scope）
curl -X POST http://localhost:5555/api/admin/organizations \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"slug":"acme","name":"ACME Corp","logo_uri":"https://acme.example/logo.svg","primary_color":"#5b2fd1"}'
```

## 3. 现在能买到什么

- **归属记账**：哪个人属于哪家公司、哪个客户端应用属于哪家公司——管理 API
  与 SQL（成员表；`users.org_id` 是已废弃的只读遗留列）均可查。
- **品牌目录**：按组织存 logo 与主色，前端可按租户换肤登录体验。
- **授权链中的组织上下文（v1.4.0）**：授权端点接受可选的 `org_id`（id 或 slug），
  由组织上下文门校验——请求者必须是有效（未软删除）的成员，客户端必须属于该组织
  或持有有效的组织授权，且启用了 `require_mfa` 的组织拒绝仅密码认证的会话。
  该绑定沿授权码→令牌链流转，携带 `org` scope 签发的令牌带有 **`org_ctx` 声明**
  （userinfo / id_token），成员资格一旦终止即不再携带。
- **组织授权（organization consent，v1.4.0）**：成员授权组织拥有的应用时，授权记录在
  **组织**上（`organization_consents` 中的管理级记录）；owner/admin 可在门户的"组织授权"页
  查看并撤销（`GET /api/me/organizations/{slug}/consents`、
  `DELETE /api/me/organizations/{slug}/consents/{clientId}`）。
- **授权申请工作流（consent request，v1.4.0，#236）**：非 owner 成员可提交组织授权
  **申请**（`organization_consent_requests`）；管理者在门户中批准/拒绝，提交申请的成员
  可撤回自己待处理的申请，获批的申请即成为组织授权。
- **Owner 继任（v1.4.0）**：owner 可提名继任者（提名 → 接受，两步确认），被软删除的
  owner 席位会依据待处理的提名自动继任，没有合格继任者的组织则由上文管理端的
  transfer-ownership 端点解围。
- **组织锚定的客户端凭据（v1.4.0）**：组织拥有的应用的 client_credentials 令牌携带
  `org_id` 声明（subject 仍是 client id）。
- **无迁移断崖**：一切都是可选、增量式的；不关心组织的部署完全不碰它。

### 3.1. 授权决定、重试与恢复

`POST /api/me/organizations/{slug}/consent-requests/{id}/approve` 在一个事务中
提交注册 scope 快照、目标申请的决定和当时可见的同组待处理申请。成功响应确认整组提交，
不存在还要等待后台写入的尾部 scope。注册 scope 为空时仍提交决定。拒绝使用相同串行化
边界，并在提交前复核 pending 状态。

事务内重新确认组织未删除，执行审批的用户仍是 owner/admin 成员。组织生命周期和同组织
的申请决定串行化；授权写入和整组撤销共用现有 `orgconsent:<org>:<client>` 锁。
成员移除、降权或申请撤回不会让过时的前置检查变成新授权。scope 集合取自事务内读取的
快照，不额外承诺与其他客户端编辑流程全局隔离。组织授权联集和 owner 继任政策保持
不变；撤销影响未来授权，不撤销已经签发的令牌。

重复批准时，只要已有任一 active 授权，就返回 200 no-op，不补入原决定之后新增的
注册 scope。因此，历史上只落地部分 scope 的授权也不会被默默扩展。受控修复由当前
manager 先检查客户端当前 scope，使用现有 DELETE 授权 API 明确撤销该组织/客户端的
整组授权，再明确重批原 approved 申请。零 active 行时，重批恢复当前 scope 快照且
不产生重复行。这是两个独立操作，中间可以没有授权；失败重试仍必须通过当前 manager
权限复核。

客户端已被软删的申请无法批准——渲染为与不存在/外部申请 id 相同的 404（此前批准会
成功并为已删除客户端写入授权）。拒绝与撤回不经过该客户端检查，此类申请仍可处理。

数据库错误和未确认提交返回失败。批准、拒绝、应用列表、申请撤回和整组撤销具有十秒
本地 HTTP 完成期限，并行查询错误也只经过一个响应出口。完成时同时检查实际 steady 截止时间，事件循环的
迟到 timer 不能让迟到成功通过；宿主必须运行 Drogon 事件循环。这一期限不取消也不能
证明在途提交已回滚：数据库可能完整提交或完整回滚。应检查持久化授权，并通过现有
幂等批准路径恢复，不能由 500/超时推断回滚。迟到的已确认决定仍可记录审计，但不能
二次响应。直接调用 C++ 仓储的宿主自行配置 DbClient timeout；HTTP 期限不等于新增
全 SDK 超时承诺。

## 4. 还没有的（路线图）

对利益相关方要说清楚——以下**未**实现：

1. **`issuer_override` 已存储但未生效**：按组织的 issuer（进发现文档与签发
   令牌）schema 就绪、运行时未接入。
2. 用户/客户端列表**无按组织过滤/隔离**；admin 跨组织可见。
3. **无组织级角色**：角色是全局的（RBAC），不按组织。
4. **无按组织的限流与 API key**——但创建*配额*已存在：自助建组织按用户配额
   （`max_orgs_per_user`）、组织拥有的客户端数量有配额（`max_org_apps`）、
   自助*应用*创建有限流（`creation_rate_limit_per_day`）（开放平台限额）。

如果今天就需要硬租户隔离：每租户跑一套 fulla 栈——Docker Compose / Helm
路径让这很便宜（[部署](../operate/deployment.md)）。

## 5. Schema 参考

组织层横跨多条迁移——权威 DDL 位于
[`apps/server/migrations/`](https://github.com/voidvec/fulla/tree/master/apps/server/migrations)：

| 迁移 | 内容 |
|---|---|
| `V017__multi_tenant.sql` | `organizations` 核心表（索引 `users(org_id)`、`organizations(slug)`） |
| `V034` | `organization_members` / `organization_invitations` |
| `V036` | `organization_consents` + `organizations.require_mfa` + 授权链上的组织列 + 删除从未使用的 `oauth2_clients.org_id` |
| `V037` | `organization_succession_nominations` |
| `V038` | `organization_consent_requests` |
| `V042` | `organizations.deleted_at` 软删除标记 + 未删除 slug 的部分索引（支撑管理端 DELETE 级联） |

`users.org_id` 是只读遗留列，计划 v2.0 物理删除。
存储层细节见[数据与持久化](../architecture/data-persistence.md)。
