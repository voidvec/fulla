# RBAC 权限控制（Role-Based Access Control）

fulla 的访问控制是一个由**三道门**强制执行的**三层模型**（v1.6.0 起，P1-8，
[ADR-0014](https://fulla.dev/docs/adr/ADR-0014-rbac-permission-enforcement) 含 Erratum 2）：
角色是粗粒度层，OAuth2 资源 scope 是细粒度层，而角色的**权限集**——两层之间的
桥——是该角色可携带的资源 scope 子集。本页描述该模型、schema、管理 API、
三道门各自的判定逻辑，以及配置与运维要点。

> **先读这一段**：角色不再只是「是不是 admin」。`admin` 角色仍然全量放行，
> 但任何自定义角色都可以被授予资源 scope 权限的一个子集（委托管理——例如
> 只能读用户的 helpdesk 角色）。反过来，持有角色永远不会绕过 token-scope
> 或同意（consent）层：权限授予的是**持有某个 scope 的资格**，token 本身
> 仍必须携带该 scope。

## 1. 核心概念

| 层 | 是什么 | 示例 | 存在于 |
|---|---|---|---|
| **角色**（粗） | 挂在用户身上的命名主体分组（M:N） | `admin`、`user`、`helpdesk` | `roles` / `user_roles` |
| **资源 scope**（细） | OAuth2 scope 词汇表；细粒度授权面 | `users:read`、`clients:write`、`audit:read` | `oauth2_scopes`（V023 家族）+ `ResourceScopeRegistry` |
| **权限**（桥） | 角色持有某一个资源 scope 的资格——权限名即资源 scope 名 | `helpdesk` 角色上的 `users:read` | `permissions` / `role_permissions` |

关系模型：

- `User <-> Role`：多对多（`user_roles`）；注册时分配默认 `user` 角色，
  日常管理走 `PUT /api/admin/users/{userId}/roles`。
- `Role <-> Permission`：多对多（`role_permissions`）；即角色的资源 scope
  集，经 `PUT /api/admin/roles/{roleId}/permissions` 管理。
- 特殊权限 **`admin`** 是超级权限：`admin` 角色被播种为它加上全部资源
  scope，且字面 `admin` 角色无论授予情况如何都直通三道门（链 ① 保留
  `hasAdminRole` 作为独立的旁路）。

## 2. 数据库 Schema（V005 + V043）

`V005__rbac_schema.sql` 建了四张 RBAC 表：

```sql
roles              (id, name UNIQUE, description, ...)          -- 播种：'admin'、'user'
permissions        (id, name UNIQUE, description, ...)
user_roles         (user_id FK users,  role_id FK roles,  PK(user_id, role_id))
role_permissions   (role_id FK roles, permission_id FK permissions, PK(role_id, permission_id))
```

`V043__rbac_permissions_seed.sql`（v1.6.0）向权限目录播种九个 V023 资源
scope 名加 `admin`，并给 `admin` 角色授予全量集合：

| 权限 | 描述（播种值） |
|---|---|
| `users:read` | 读取用户账号与角色 |
| `users:write` | 创建、更新、删除用户；分配角色 |
| `clients:read` | 查看 OAuth2 客户端注册 |
| `clients:write` | 创建、更新、删除 OAuth2 客户端 |
| `tokens:read` | 查看已签发的令牌 |
| `tokens:write` | 吊销与管理令牌 |
| `roles:read` | 查看角色与 scope 目录 |
| `roles:write` | 创建与更新角色、scope 及其映射 |
| `audit:read` | 读取与导出审计日志 |
| `admin` | 管理超级权限（蕴含全量集合） |

注意：

- 四个 V005 遗留名（`user:read`、`user:write`、`user:delete`、
  `admin:access`）已**原地退役**——V043 用 `[LEGACY: ...]` 标记改写其
  描述（UPDATE，不 DELETE）。执行层只读 V023 形状的名字，因此遗留行
  （例如 `user` 角色上的旧 `user:read` 授予）按设计是惰性的；请用新名
  重新授予。
- **内存模式**（`storage_type: memory`）没有 `role_permissions` 行；
  `RolePermissionResolver` 退回到镜像 V043 播种的编译期默认映射：
  `admin` → 全量十项权限，其它角色 → 空。

## 3. 管理 API

以下路由均为 admin 门控（`roles:read` / `roles:write` / `users:write`
scope，各自带 `impliedBy: admin`）——见 [API 参考](api-reference.md)：

| 方法与路径 | 用途 |
|---|---|
| `GET /api/admin/roles` | 列出角色及用户数（`roles:read`） |
| `POST /api/admin/roles` | 创建角色——内置角色（`admin`、`user`）不可重复创建（`roles:write`） |
| `PUT /api/admin/roles/{roleId}` | 更新角色描述（`roles:write`） |
| `DELETE /api/admin/roles/{roleId}` | 删除角色——内置角色不可删除（`roles:write`） |
| `GET /api/admin/roles/{roleId}/permissions` | 该角色的权限名列表（`roles:read`） |
| `PUT /api/admin/roles/{roleId}/permissions` | **全量替换**该角色的权限集（`roles:write`） |
| `GET /api/admin/users/{userId}/roles` | 用户的角色（`users:read`） |
| `PUT /api/admin/users/{userId}/roles` | 分配用户角色——若会剥掉最后一个活跃 admin 则 409（`users:write`） |
| `GET /api/admin/scopes` | OAuth2 scope 目录（`roles:read`） |
| `GET /api/admin/scopes/resources` | (path, method) → 所需 scope 矩阵（见 §5） |

**PUT permissions 语义**：请求体 `permissions` 数组即该角色的完整授予
集。每个列出的名字必须已存在于权限目录——一个未知名就使整个替换以
400 失败（不会部分授予）。变更在事务内完成，审计为
`role_permissions_updated`，并在提交后失效角色-权限缓存条目（单实例部署
即时一致；多实例漂移是已知限制，待 P3 共享存储解决）。

**首个 admin**：`auth.bootstrap_admin.enabled`（默认 `true`）时，服务器
在启动阶段发现尚无用户持有 admin 角色，则创建 `admin` 用户——密码取自
`FULLA_BOOTSTRAP_ADMIN_PASSWORD`，否则随机生成并仅在日志打印一次（见
[配置指南 §2](../operate/configuration-guide.md)）。无论首个 admin 还是
权限授予，都不再需要手工 SQL。

示例——给 helpdesk 角色授予只读用户权限：

```bash
# 1. 创建角色（token：admin scope）
curl -X POST http://localhost:5555/api/admin/roles \
  -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
  -d '{"name":"helpdesk","description":"Read-only user support"}'

# 2. 精确授予一项权限（全量替换）
curl -X PUT http://localhost:5555/api/admin/roles/3/permissions \
  -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
  -d '{"permissions":["users:read"]}'

# 3. 挂到某个用户身上（角色列表全量替换）
curl -X PUT http://localhost:5555/api/admin/users/5/roles \
  -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
  -d '{"roles":["user","helpdesk"]}'
```

## 4. 执行——三道门

授权判定发生在三个独立位置。v1.6.0 把三者都改接到权限查询上；漏掉任何
一个，委托管理员都会因请求路径不同而得到不一致的判定。

| 门 | 组件 | 判定什么 |
|---|---|---|
| ① scope 分层 | `ScopeDecisionEngine` 谓词链（libs/oauth2） | 该用户**请求的 scope** 能否被签发/使用 |
| ② 身份检查 | `IdentityService::validateUserRolesForScopes`（libs/drogon services） | 登录/两步流程上的同一问题（及所有直接调用方） |
| ③ HTTP 角色门 | `AuthorizationFilter::checkAccess`（libs/drogon filters） | 这条 **HTTP 请求**本身能否放行 |

**门 ①——scope 分层（签发）**。在 `/oauth2/authorize`（及换 token）处，
每个请求的 scope 走三层：客户端允许清单 → admin 档检查 → 同意。哪些
scope 属于 *admin 档*来自数据库（`oauth2_scopes.requires_admin_role`，
启动时由 `OAuth2Plugin` 加载进 `AuthorizationService` 与
`IdentityService`——弃用列，读到 v2.0）。用户是否*通过*则基于权限：
admin 档 scope 在用户的角色→权限并集覆盖它时通过，**或者**用户持有字面
`admin` 角色。分类与判定刻意解耦——授予权限永远不会让一个 scope 降档。

**门 ②——IdentityService**。登录路径（`/oauth2/login` 与两步门户流程）
在签发授权码之前用同一套逻辑对请求的 scope 分层：admin 档 scope 要求
字面 `admin` 角色，或权限并集覆盖**每一个**请求的 admin 档 scope。

**门 ③——AuthorizationFilter（每请求）**。在每条 admin 路由上，过滤器：

1. **只**从 `Authorization: Bearer` 头提取 token（v1.6.0 移除了
   query/form 回退——RFC 9700 禁止）。
2. 校验 access token（不透明引用 token 对照存储做内省），并把主体
   （`userId`、`scope`、`clientId`、`orgId`）钉到请求属性上。
3. **先查 scope 门**：在 `ResourceScopeRegistry`——集中的
   (path, method) → 所需 scope 矩阵——里查该路由，token 的 scope 集
   不满足则以 `403 insufficient_scope` 拒绝（附 RFC 6750
   `WWW-Authenticate` 挑战）。
4. **角色/权限门**：从数据库重新解析用户角色（token 响应里的 roles
   仅供展示——访问判定绝不信任它），经 `RolePermissionResolver` 解析
   角色→权限并集，再拿请求路径匹配 `rbac_rules`（§5）。失败 → `403`
   `AUTHZ_INSUFFICIENT_PERMISSIONS`；没有规则命中且不是公开路径 →
   **DEFAULT DENY**。

## 5. Scope 体系：OIDC scope 与资源 scope

`oauth2_scopes` 里并存两个 scope 家族：

- **OIDC / 身份 scope**——`openid`、`profile`、`email`（外加 v1.4.0 的
  `org`）：描述客户端能看到哪些身份数据。由用户同意授予；永不属于
  admin 档。
- **资源 scope**——V023 家族（`users:read` … `audit:read`，全部
  `requires_admin_role = TRUE`）：门控 admin API 面，`ResourceScopeRegistry`
  中每条路由一个 (path, method) 条目。

实践中重要的蕴含规则：

- 携带 **`admin`** 的 token 满足任何资源 scope 要求——每条 admin 路由都
  声明 `impliedBy: ["admin"]`（蕴含按路由声明，不是全局图）。
- `admin` **永不**蕴含 `openid` / `profile` / `email`——这些要求带空的
  `impliedBy`，所以 userinfo 调用永远需要一枚真正携带 `openid` 的用户
  token。同样，`/api/me` 家族要求 `profile` 且**无** impliedBy：admin
  token 不能满足用户自助服务。
- 角色的权限集决定该用户**可以被签发**哪些资源 scope；token 仍必须携带
  该 scope（客户端请求它、门 ①/② 校验它、同意被记录）。

发现实时矩阵（只读，无 DB 访问）：

```bash
curl http://localhost:5555/api/admin/scopes/resources \
  -H "Authorization: Bearer $ADMIN_TOKEN"
# → {"resources":[{"path":"/api/admin/users","method":"GET",
#    "required_scopes":["users:read"],"implied_by":["admin"]}, ...]}
```

注册表在启动时从各控制器的 OpenAPI 端点声明一次性构建；启动一致性检查
会在 admin 门控路由缺少条目时中止进程（防静默缺口）。

## 6. 委托管理员（v1.6.0）

三道门模型解锁了 v1.6 之前「字面 admin 角色」门无法表达的能力：**持有
受限权限子集的自定义角色**。完整示例——§3 那个只持 `users:read` 的
`helpdesk` 角色：

- `GET /api/admin/users` 可用：门 ③ 的 `/api/admin/users.*` 规则
  （`{"permissions":["users:read"]}`）先于 `/api/admin/.*` 兜底规则命中；
  门 ① 允许为 helpdesk 用户签发 `users:read`（token 必须携带它且同意已
  记录）。
- 其它一切 admin 操作都被拒绝：兜底 `"/api/admin/.*": ["admin"]` 要求
  字面角色，且门 ①/② 拒绝其它 admin 档 scope（没有权限覆盖它们）。
- 红线（安全评审 H2）：helpdesk 授予永远不会让 scope *降档*——
  `users:write` 仍是 admin 档；helpdesk 只是在它的权限检查上失败。

运维注意：

- 权限与角色-权限写入都走管理 API，提交时失效进程内角色-权限缓存——
  单节点部署即时生效；多实例部署的其它节点在重启前会继续提供过期授予
  （已知限制，由 P3 共享存储工作包解决）。
- 内存模式部署使用编译期默认映射（§2），无法表达委托角色。

## 7. 配置速查

`rbac_rules` 位于 `config.json` 的 `custom_config` 下，把**正则路径模式**
映射到要求。出厂默认（`apps/server/config.json`）同时展示了两种形式：

```json
"rbac_rules": {
    "/api/admin/users.*": { "permissions": ["users:read"] },
    "/api/admin/.*": ["admin"],
    "/api/user/.*": ["user", "admin"],
    "/oauth2/register": ["admin"],
    "/oauth2/device/approve": ["admin"]
}
```

- **数组形式**（遗留，语义不变）：角色名 OR——持有列表中任一角色即通过。
- **对象形式** `{"roles": [...], "permissions": [...]}`（v1.6.0）：调用方
  的角色→权限并集必须覆盖**每一个**列出的权限名；只写 permissions 的
  条目就是委托管理形态。
- **匹配**：最具体优先——加载时按模式长度降序排序，**第一个**命中路径
  的规则生效。把具体规则放在兜底规则之前（加载器已强制如此，但 JSON
  里也建议保持这个顺序以便阅读）。
- **DEFAULT DENY**：不命中任何规则、又不在 `public_paths`（可选的正则
  数组，默认未设置）里的路径一律拒绝。
- 出厂的 `/api/user/.*` 条目是遗留残迹——`/api/user/` 下没有任何注册
  路由（面向用户的家族是 `/api/me/*`，由 `OAuth2AuthFilter` 用 `profile`
  scope 门控，不走 rbac_rules）。无害，但不要把它当模板抄。

相关阅读：[配置指南 §15](../operate/configuration-guide.md)（RBAC 执行
开关）、[多租户](multi-tenancy.md)（角色为何是全局的、不按组织划分）、
[架构总览](../architecture/architecture-overview.md)。
