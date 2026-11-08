---
adr: 0014
title: RBAC 权限执行 —— 权限即角色的资源-scope 集，取代两道 admin-scope 门
date: 2026-10-11
status: Accepted
---

## 背景

RBAC schema（`V005`：`roles` / `permissions` / `user_roles` / `role_permissions`）带着种子数据上线，却**零执行**——没有任何业务代码读 `role_permissions` 来做授权决策（只有 drogon_ctl 生成的 ORM 样板引用它）。`docs/domains/rbac-guide.md:11` 自述该模型为"简化为基于角色的 URL 拦截"。2026-10 的 IAM 差距分析将其定性为架构性误导（评估者读到 schema 会以为存在一个可工作的权限引擎）。

实际把守管理操作的是**两条并行、彼此独立**的链：

1. **DB 列链**：`OAuth2Plugin` 启动时把 `oauth2_scopes.requires_admin_role` 列加载进一个注入的谓词（`OAuth2Plugin.cc:643-674`；P1-8 勘误：最初的 566-585 锚点指向的是 Redis 缓存代码）；`ScopeDecisionEngine.cc:31-36` 消费它（`scopeRequiresAdmin(scope) && !hasAdminRole → admin_role_required`）。引擎本身从不读该列，`AuthorizationService.cc:42-56` 还保有一份硬编码的 `defaultAdminScopes()` 回退集合，仅在 DB 无数据时使用。
2. **硬编码链**：`IdentityService::scopeRequiresAdminRole`（`IdentityService.cc:227-240`）写入一个字面集合 `{admin, admin:read, admin:write, user:manage, settings:manage}`（带前缀匹配），由 `validateUserRolesForScopes`（`:188-224`）消费并自 `OAuth2Plugin.cc:1418-1420` 转发（P1-8 勘误：原锚点 1254-1256 已漂移）。

因为两条链都要求字面的 `admin` **角色**，委托管理员——例如一个只允许读用户的"helpdesk"角色——根本无法表达。决策 D2（2026-10-10，路线图 `deliverables/iam-improvement-roadmap-2026-10.md` §8）选择**执行而非冻结/移除**，并附"集成、不复制"的约束。

## 决策

1. **语义**：一行 `permissions` 被重新定义为**一个角色持有某资源 scope 的权利**。权限的名字就是资源 scope 的名字（`users:read`、`clients:write`、`audit:read`、……）——即 `V023__resource_scopes.sql` 已经播种的词汇表。不发明新的权限语法；细粒度授权面仍然是 OAuth2 scope 模型（scope 目录 + `ResourceScopeRegistry` 的 (path,method)→scope + consent），与今天完全一致。`role_permissions` 变成"哪些资源 scope 随此角色生效"。
2. **集成——两条链都要，而非其一**：P1 实现把**每条**链里的 admin 检查替换为同一个查找：
   - 链 1：注入 `ScopeDecisionEngine` 的谓词变为"该用户是否经其**任意**角色持有一个名字等于所请求 scope 的权限（或 `admin`——它仍是蕴含一切资源 scope 的超级 scope）"。
   - 链 2：删除 `IdentityService::scopeRequiresAdminRole` 及其字面集合；`validateUserRolesForScopes` 改为查询同一权限查找。跳过链 2 会让委托管理员的判定结果取决于请求走了哪条路径（2026-10-10 评审标记的失败模式）。
3. **兼容种子**：一条迁移把目录中已有的全部资源 scope 授予 `admin` 角色，admin 行为前后逐字节一致；`requires_admin_role` 列与硬编码集合都标记弃用（列不早于 v2.0、在弃用窗口之后删除）。
4. **解锁的能力——委托管理员**：自定义角色可被授予子集（如 helpdesk 仅 `users:read`）。管理控制台的角色页新增 角色→scope 集 分配界面。验收（P1-8）：一个仅持 `users:read` 的全新"helpdesk"角色能且只能列出用户；`admin` 角色行为与之前完全一致。
5. **api-diff 约束**：minor 版本不允许 `IMfaRepository` 式的接口拓宽；查找位于既有 `IRoleProvider`/`IAuthorizationService` 缝合点之后（扩展**具体**的 `StorageRoleProvider` / `AuthorizationService`，若确需新缝合点则在 `common` 加一个小端口——仅限增量）。

## 非目标（明确不在 P1-8 范围内）

- 角色层级、角色→客户端绑定、组织范围角色（P1-8 落地后重审；路线图 §11 I12）。
- ABAC / 通用策略引擎。
- 改动 `ResourceScopeRegistry` 的映射内容或 consent 的工作方式。

## 结果

- `role_permissions` 不再是死 schema，而成为委托管理员的机制；`rbac-guide.md` 重写为三层模型（角色 = 粗粒度，scope = 细粒度，角色→scope 集 = 桥）。
- 两个弃用门（`requires_admin_role` 列、`IdentityService` 字面集合）收缩为一个版本的空转垫片，随后移除。
- 实现作为路线图工作包 P1-8（v1.6.0）跟踪；本 ADR 是 issue/PR 引用的设计契约。

---

## 勘误 2 —— P1-8 执行（三道门模型，2026-10）

原 ADR 描述了两条链。P1-8 的对抗性评审（三位独立评审人）确立真实拓扑是**三道门**：上述两条 scope 链**加上** HTTP 层的角色门 `AuthorizationFilter::checkAccess`（rbac_rules 正则 → 字面角色，DEFAULT DENY）——没有它，委托管理不可达（helpdesk 用户在任何 scope 逻辑运行之前就被 403）。已交付的 P1-8 执行：

1. **分类与判定解耦**（安全评审 H2）：`requires_admin_role` 仍是 admin-**类**的来源（已弃用，读到 v2.0）；用户**是否通过**则基于权限（角色→权限的并集覆盖该 scope，或字面 admin 超级角色）。
2. **链①**：引擎谓词现在是 `scope ∈ admin 类 ∧ scope ∉ 用户权限集 → invalid`（每请求闭包；引擎签名不变）。
3. **链②**：`IdentityService::scopeRequiresAdminRole` 读 DB 加载的 admin-类集合（签名不变）；`validateUserRolesForScopes` 中的判定基于权限。
4. **门③**：rbac_rules 条目接受 `{"permissions": [...]}`；最具体优先匹配；catch-all 与 DEFAULT DENY 基线不动。
5. 两步登录路径现在经同一引擎对 scope 分层（此前它签发未过滤的授权码）。
6. V043 播种权限目录（V023 的九个名字 + admin）；V005 的四个遗留名字原地退役（M4：UPDATE，而非 DELETE）。
