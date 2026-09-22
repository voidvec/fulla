---
sidebar_position: 7
---

# 多租户（Organizations）

fulla 的多租户目前是一个**组织层**：组织归组用户与客户端、承载品牌字段、
经管理 API 管理。本页如实记录现状有什么、**没有**什么、以及如何在不过度
假设隔离的前提下使用它。

> **先读这段**：组织是元数据与归属归组，不是硬隔离边界。授权由 RBAC +
scope 强制（见 [RBAC 指南](rbac-guide.md)）；今天属于某组织的主体并不会
自动与其他组织的数据隔离。

## 1. 模型（V017）

```sql
organizations (
    id              SERIAL PRIMARY KEY,
    slug            VARCHAR(50) UNIQUE,   -- 3–50 字符，小写
    name            VARCHAR(200),
    logo_uri        VARCHAR(512),         -- 品牌
    primary_color   VARCHAR(7),           -- 品牌
    issuer_override VARCHAR(512),         -- 已存储；见 §4 路线图
    created_at / updated_at
)
```

两个可空外键把实体挂到组织上：

| 列 | 所在表 | 语义 |
|---|---|---|
| `org_id` | `users` | V017 的单组织锚点，已废弃。**v1.5.0 起只读**（admin API 写入面已移除）；`NULL` = 未设置。计划 v2.0 物理删除。 |
| （已删除） | `oauth2_clients` | V017 的 `org_id` 列从未被代码读写，**V036 已 DROP**。客户端归属在 `oauth2_client_owners.org_id`（v1.4.0）。 |

v1.5.0 起组织归属的唯一权威锚点是 M:N 成员表（`organization_members`）与
`oauth2_client_owners.org_id`；`users.org_id` 只是为迁移保留的只读遗留列。

## 2. 管理 API 面

所有路由要求 admin scope 令牌（`AuthorizationFilter`；`impliedBy: admin`）——
[API 参考](api-reference.md)客户端管理一节：

| 方法与路径 | 用途 |
|---|---|
| `GET /api/admin/organizations` | 列出组织（id、slug、name、品牌字段） |
| `POST /api/admin/organizations` | 创建（slug：3–50 个小写字符，唯一） |
| `GET /api/admin/organizations/{slug}` | 查询单个 |

另：

- 管理用户 API（`POST/PUT /api/admin/users`）**不再接受 `org_id`**（v1.5.0
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
- **无迁移断崖**：一切都是可选、增量式的；不关心组织的部署完全不碰它。

## 4. 还没有的（路线图）

对利益相关方要说清楚——以下**未**实现：

1. **`issuer_override` 已存储但未生效**：按组织的 issuer（进发现文档与签发
   令牌）schema 就绪、运行时未接入。
2. 用户/客户端列表**无按组织过滤/隔离**；admin 跨组织可见。
3. **无组织级角色**：角色是全局的（RBAC），不按组织。
4. 组织**无 update/delete** 端点（只有 create/list/get）。
5. **无按组织的限流、配额、密钥**。

如果今天就需要硬租户隔离：每租户跑一套 fulla 栈——Docker Compose / Helm
路径让这很便宜（[部署](../operate/deployment.md)）。

## 5. Schema 参考

权威 DDL 是
[`V017__multi_tenant.sql`](https://github.com/voidvec/fulla/blob/master/apps/server/migrations/V017__multi_tenant.sql)
（V017 在 `users(org_id)`、`oauth2_clients(org_id)`、`organizations(slug)` 上
建了索引；`oauth2_clients.org_id` 列及其索引已在 V036 删除，`users.org_id`
计划 v2.0 删除）。
存储层细节见[数据与持久化](../architecture/data-persistence.md)。
