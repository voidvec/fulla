---
sidebar_position: 8
---

# 自定义声明（Claims Mapping）

自 **v1.6.0**（P1-3，#280）起，fulla 可以把**按用户维护的自定义声明**发布进
`id_token` 与 `userinfo`：用户在自助端维护 `custom_attributes` 文档，管理员把
一个来源路径映射为**触发 scope** 之下的已发布声明——即 Keycloak 协议映射器的
最小等价物。本页记录模型、两个 API 面、安全红线（及其成因）、发布语义与缓存行为。

> **先读这段**：这个特性是刻意收窄的。映射只能读
> `custom_attributes.<key>` 或内置的 `user.display_name`，永远不能遮蔽标准声明，
> 且以 RP 显式请求的 scope 为门。如果你需要任意声明变换或联邦协议映射器，
> 那是设计上就不做的（见 §7）。

## 1. 模型（V044）

一条迁移交付整个特性——`V044__custom_attributes_claim_mappings.sql`：

```sql
-- 用户侧来源数据（自助、整体替换）
ALTER TABLE users ADD COLUMN IF NOT EXISTS custom_attributes JSONB
    NOT NULL DEFAULT '{}';
CREATE INDEX idx_users_custom_attributes_gin ON users USING GIN (custom_attributes);

-- 管理侧发布规则
CREATE TABLE claim_mappings (
    id SERIAL PRIMARY KEY,
    scope_name  VARCHAR(64)  NOT NULL,          -- 触发 scope（禁止 'openid'）
    claim_name  VARCHAR(64)  NOT NULL,          -- 发布的声明名
    source_path VARCHAR(128) NOT NULL,          -- custom_attributes.<key> | user.display_name
    include_in  VARCHAR(16)  NOT NULL DEFAULT 'both',  -- id_token | userinfo | both
    enabled     BOOLEAN      NOT NULL DEFAULT TRUE,
    CONSTRAINT uq_claim_mappings UNIQUE (scope_name, claim_name)
);
```

一条映射就是一条规则：*当令牌/userinfo 请求携带 `scope_name` 时，把
`source_path` 处找到的值发布为 `claim_name` 声明，发布面由 `include_in`
选择*。唯一约束保证每个 scope 下一个声明名只出现一次；同一声明名挂到别的
scope 是另一行。

仅限 Postgres——没有内存态 claim-mapping 存储：自助面在**内存存储部署上答
404**（自定义属性是 claims-mapping 的来源数据，不是资料字段）；管理端 CRUD
需要数据库客户端，缺失时答 `DB_CONNECTION_ERROR`。

## 2. 自助面：custom-attributes 文档

| 端点 | 用途 |
|---|---|
| `GET /api/me/profile/custom-attributes` | 返回调用者的文档（`{status, custom_attributes}`）。 |
| `PUT /api/me/profile/custom-attributes` | **整体替换**文档（不做合并）。 |

写入校验（`CUSTOM_ATTRIBUTES_INVALID`，错误码 3026，HTTP 400）：

- 必须是 JSON **对象**（不能是数组/标量）；
- **深度 ≤ 2**（文档根加一层嵌套）；
- 序列化后 **≤ 8 KiB**；
- 每个键匹配 `^[a-z][a-z0-9_]{0,63}$`；
- 原型污染三件套（`__proto__` / `constructor` / `prototype`）直接拒绝
  （正则本就排除它们；显式检查是为固化意图、防正则回归）。

```bash
curl -X PUT https://fulla.example.com/api/me/profile/custom-attributes \
  -H "Authorization: Bearer $USER_ACCESS_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"department": "platform", "employee_id": "E-4102", "prefs": {"locale": "zh-CN"}}'
```

整体替换语义意味着客户端应当 GET、改、再 PUT 全量文档。值可以是任意 JSON
类型，但**只有标量会发布**为声明（§5）——对象/数组可以存，永不发出。

## 3. 管理 API：claim mappings

管理门控（`GET/POST /api/admin/claim-mappings`、
`PUT/DELETE /api/admin/claim-mappings/{mappingId}`）；**暂无管理控制台页面**，
CRUD 仅经 API。

| 操作 | 端点 | 说明 |
|---|---|---|
| 列表 | `GET /api/admin/claim-mappings` | `{status, total, mappings[]}`，含全部五个字段。 |
| 创建 | `POST /api/admin/claim-mappings` | 请求体：`scope_name`、`claim_name`、`source_path`（必填），`include_in`（`id_token` \| `userinfo` \| `both`，默认 `both`）。201。 |
| 更新 | `PUT .../{mappingId}` | 全量请求体，外加 `enabled` 用于启停。未知 id 404。 |
| 删除 | `DELETE .../{mappingId}` | 未知 id 404。 |

```bash
curl -X POST https://fulla.example.com/api/admin/claim-mappings \
  -H "Authorization: Bearer $ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"scope_name": "hr", "claim_name": "employee_id",
       "source_path": "custom_attributes.employee_id", "include_in": "both"}'
```

拒绝以结构化 VALIDATION 错误返回（HTTP 400）：

| 错误 | 码 | 含义 |
|---|---|---|
| `CLAIM_MAPPING_RESERVED_CLAIM` | 3023 | `claim_name` 撞保留字黑名单（§4）。 |
| `CLAIM_MAPPING_FORBIDDEN_SCOPE` | 3025 | `scope_name` 为 `openid`。 |
| `CLAIM_MAPPING_INVALID_SOURCE` | 3024 | `source_path` 不在白名单（§4）。 |

## 4. 安全红线——及其成因

设计稿的对抗性评审发现了一条**接管链**（管理员把映射挂到 `email`/`sub` 上，
即可改写 RP 所信任的身份断言）。落地时的封堵，在每次创建/更新的**写入时**强制：

1. **保留声明黑名单**——`claim_name` 永不得遮蔽服务端已发布或 OIDC 保留的声明：
   `sub, iss, aud, exp, iat, nonce, auth_time, acr, amr, org_ctx, email,
   email_verified, name, preferred_username, family_name, given_name,
   nickname, picture, website, phone_number, phone_number_verified,
   username, roles`。
   此外 `claim_name` 必须匹配 `^[a-z][a-z0-9_]{0,63}$`。
2. **来源白名单**——恰好两个：`custom_attributes.<key>`（键受上节自助语法约束）
   与唯一的内置 `user.display_name`。设计稿中的 `org.role` 来源被**砍掉**：
   经用户可编辑通道发布角色类数据是跨上下文泄漏。
3. **禁止 `openid` 触发**——`openid` 无条件授予，挂在它上面等于零授权发布。
   请选一个 RP 必须实际请求的 scope（目录 scope 或自定义 scope）。

## 5. 发布语义

- **触发 scope 门**：仅当请求的 scope 集合包含 `scope_name` 时映射才生效
  （精确匹配，标准 OAuth2 scope 令牌语义）。裸 `openid` 令牌什么也不发布。
- **发布面**（`include_in`）：`id_token`（授权码路径签发，**且每次刷新轮换时
  重估**——轮换遗留刷新族会重新计算映射声明）、`userinfo`，或两者。
- **仅标量**：来源解析只在路径存在**且**解析为标量时发布值。路径缺失、对象、
  数组 → 该用户的这个声明直接省略（绝不报错）。
- **发现**：已启用映射的声明名进入两个元数据端点
  （`.well-known/openid-configuration` 与 `oauth-authorization-server`）的
  `claims_supported`，经与其余目录相同的缓存钩子刷新——见 §6。

标准 scope 门控在下面照常生效（v1.5.0）：`name` / `preferred_username` /
`picture` 需 `profile`，`email` / `email_verified` 需 `email`。映射声明是
叠加，永不放宽这些行为。

## 6. 缓存行为（热路径安全）

第一版实现在**每次** userinfo 与签发调用时重查 `claim_mappings`——v1.6.0
五场景基准门禁实测让受影响场景比 v1.5.0 损失 17–18% QPS。落地修复：

- `StorageClaimMappingsProvider` 被 `CachedClaimMappingsProvider` 包裹
  （进程本地缓存）。**TTL 只约束跨进程漂移**——每条 claim-mapping 写路径立即
  失效缓存，同一钩子刷新 discovery 的 `claims_supported`，因此 CRUD 即刻可见
  （由 `ClaimsMapping_PublicationChain` 集成测试验证：创建 → 下一次
  令牌/userinfo 携带该声明；删除 → 下一次调用不再携带）。

内省的组织覆盖在同一个版本里得到同样处理（3s → 60s TTL 插件缓存，
从两处 owner 行写路径精确失效）。

## 7. 限制（设计使然）

- 没有变换、拼接、多值映射——来源路径是标量时原样复制。
- 除 `custom_attributes.<顶层键>` 外不做更深路径解析：白名单语法在前缀后
  恰好一段（嵌套数据随便存；发布顶层标量）。
- 没有按客户端或按组织的映射覆盖；映射是全局的，以触发 scope 为键。
- 仅限 Postgres 部署——内存模式下自助面答 404、管理端 CRUD 答
  `DB_CONNECTION_ERROR`。

## 8. 端到端示例

1. 用户存入属性（§2 的 curl）。
2. 管理员创建映射（§3 的 curl）——触发 scope `hr`、声明 `employee_id`、
   来源 `custom_attributes.employee_id`、双面发布。
3. RP 在授权请求中带 `scope=openid hr`（该 scope 必须存在于目录且对该客户端
   放行）。
4. 结果——`id_token` 载荷新增：

```json
{
  "sub": "0192f0a4-…",
  "employee_id": "E-4102",
  …
}
```

   且 `GET /oauth2/userinfo`（同一令牌）携带 `"employee_id": "E-4102"`。
5. `/.well-known/openid-configuration` 的 `claims_supported` 现在列出
   `employee_id`——RP 可以发现这个扩展声明而不必硬编码。

关于 scope 目录本身（创建/放行 `hr` 触发 scope）见 API 参考
（`/api/admin/scopes*`）；标准声明的 scope 门控见
[OIDC 集成指南](oidc-guide.md)。
