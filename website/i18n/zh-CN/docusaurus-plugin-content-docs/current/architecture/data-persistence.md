# OAuth2 数据持久化文档 (Data Persistence)

本文档详细描述了 OAuth2 插件的持久化层设计、数据库 Schema、Redis 键值结构以及安全加固方案。

## 1. 设计目标

- **存储解耦**：通过仓储接口（`libs/oauth2/include/fulla/oauth2/repository/` 下的 `IClientRepository`、`IGrantRepository`、`ITokenRepository` 等）抽象，支持内存、PostgreSQL、Redis 等多种存储后端，各后端以 `*RepositoryBundle` 装配实现。
- **数据持久化**：确保 Client 信息、Token、Auth Code 等关键数据不丢失。
- **安全加固**：Client Secret 绝不明文存储，强制使用 SHA256 加盐哈希。
- **异步高性能**：底层操作全部采用 `execSqlAsync` 和 `execCommandAsync`，基于回调机制，充分利用 Drogon 的非阻塞 I/O 能力。

---

## 2. PostgreSQL 存储方案

适用于生产环境，提供严格的全部关系型数据一致性。

### 2.1 Database Schema

由迁移脚本 `apps/server/migrations/V002__oauth2_core.sql` 创建（幂等，`IF NOT EXISTS`；后续迁移会追加 scopes、device codes、lockout 等列）。核心表结构如下：

#### 客户端表 (`oauth2_clients`)

存储接入的客户端应用信息。

```sql
CREATE TABLE IF NOT EXISTS oauth2_clients (
    client_id       VARCHAR(50) PRIMARY KEY,
    client_type     VARCHAR(20) NOT NULL DEFAULT 'CONFIDENTIAL',
    client_secret   VARCHAR(100) NOT NULL, -- 存储 SHA256(secret + salt) 的 Hex 字符串
    salt            VARCHAR(50) NOT NULL,  -- 随机盐值
    name            VARCHAR(100),
    redirect_uris   TEXT,                  -- 逗号分隔或 JSON 数组
    allowed_grant_types TEXT               -- 允许的 grant_type 列表
);
```

#### 授权码表 (`oauth2_codes`)

短期有效的授权凭证。

```sql
CREATE TABLE IF NOT EXISTS oauth2_codes (
    code            VARCHAR(100) PRIMARY KEY,
    client_id       VARCHAR(50) NOT NULL REFERENCES oauth2_clients(client_id),
    user_id         VARCHAR(50),
    scope           TEXT,
    redirect_uri    TEXT,
    code_challenge  VARCHAR(128),          -- PKCE 支持
    code_challenge_method VARCHAR(10),      -- S256 / plain
    expires_at      BIGINT NOT NULL,       -- Unix Timestamp
    used            BOOLEAN DEFAULT FALSE  -- 防重放攻击
);
```

#### 访问令牌表 (`oauth2_access_tokens`)

```sql
CREATE TABLE IF NOT EXISTS oauth2_access_tokens (
    token           VARCHAR(100) PRIMARY KEY, -- 存 SHA-256(token) 哈希（64 hex），非明文（ADR-0004）
    client_id       VARCHAR(50) NOT NULL REFERENCES oauth2_clients(client_id),
    user_id         VARCHAR(50),
    scope           TEXT,
    expires_at      BIGINT NOT NULL,
    revoked         BOOLEAN DEFAULT FALSE,
    issued_at       BIGINT NOT NULL DEFAULT EXTRACT(EPOCH FROM CURRENT_TIMESTAMP)::BIGINT,
    issuer          VARCHAR(255) NOT NULL DEFAULT '',
    audience        VARCHAR(255),
    not_before      BIGINT DEFAULT EXTRACT(EPOCH FROM CURRENT_TIMESTAMP)::BIGINT,
    introspect_count INTEGER DEFAULT 0,
    revoked_at      BIGINT,
    revoked_by      VARCHAR(50)
);
```

#### 刷新令牌表 (`oauth2_refresh_tokens`)

```sql
CREATE TABLE IF NOT EXISTS oauth2_refresh_tokens (
    token           VARCHAR(100) PRIMARY KEY,   -- 存 SHA-256(token) 哈希，非明文（ADR-0004）
    access_token    VARCHAR(100) NOT NULL, -- 关联的访问令牌哈希（无外键约束，按值引用）
    client_id       VARCHAR(50) NOT NULL REFERENCES oauth2_clients(client_id),
    user_id         VARCHAR(50),
    scope           TEXT,
    expires_at      BIGINT NOT NULL,
    revoked         BOOLEAN DEFAULT FALSE,
    revoked_at      BIGINT,
    revoked_by      VARCHAR(50)
);
```

---

## 3. Redis 存储方案（已弃用）

> **⚠️ 独立 Redis 存储已弃用（F-005）**：该模式启动时打 ERROR 日志，并以
> `unsupported_grant_type` 拒绝 `refresh_token` 授权；历史上 refresh token
> 从未在该模式持久化。新部署一律用 `postgres` + 可选 Redis 缓存层（§缓存
> 一致性）。以下键空间仅作存量部署参考。

### 3.1 Key Pattern 设计

所有 Key 均以 `oauth2:` 前缀开头（缓存层另有独立的 `fulla:cache:` 前缀，
事务协调键族 `oauth2:transaction:*` 未列入下表）。

| 实体 | Key 格式 | 类型 | TTL | 说明 |
|------|-------------|------|-----|------|
| **Client** | `oauth2:client:{client_id}` | Hash | 无 | 字段: `secret` (Hash), `salt`, `redirect_uris` (JSON), `allowed_scopes` (JSON) |
| **Auth Code** | `oauth2:code:{code}` | String | 10分钟 | Value: JSON 序列化对象 |
| **Access Token** | `oauth2:token:{token}` | String | 1小时 | Value: JSON 序列化对象 |
| **Refresh Token**| `oauth2:refresh:{token}` | String | 30天 | Value: JSON 序列化对象 |

### 3.2 示例数据

**Client (Hash Structure)**:

```bash
HSET oauth2:client:fulla-portal secret "42a121b66fb9f1d4f73125788f42eb6799110c6aeae5a9a12a2fed5307a0088d" salt "random_salt" redirect_uris "[\"http://localhost:5173/callback\"]"
```

**Auth Code (String Value)**:

```json
{
  "client_id": "fulla-portal",
  "user_id": "admin",
  "scope": "openid",
  "redirect_uri": "http://localhost:5173/callback",
  "expires_at": 1735689000,
  "used": false
}
```

---

## 4. 安全加固 (Security Hardening)

为了防止数据库泄露导致 Client Secret 暴露，本系统实施了强制哈希策略。

### 4.1 算法与流程

1. **存储时**：
    - 生成随机 `salt`（可选，但在 Postgres Schema 中建议预留）。
    - 计算 `Hash = SHA256(raw_secret + salt)`。
    - 数据库存储 `Hash` (Hex String) 和 `salt`。

2. **验证时**：
    - 用户提交 `input_secret`。
    - 系统读取库中的 `stored_hash` 和 `salt`。
    - 计算 `CheckHash = SHA256(input_secret + salt)`。
    - 比对 `CheckHash` 与 `stored_hash` (忽略大小写)。

### 4.2 代码实现

位于 `RedisClientRepository::validateClient` 和 `PostgresClientRepository::validateClient` 中。

```cpp
// 核心逻辑示例
std::string input = clientSecret + client->salt;
std::string calculatedHash = drogon::utils::getSha256(input.data(), input.length());
return lower(calculatedHash) == lower(storedHash);
```

---

## 5. 数据生命周期管理 (Data Lifecycle)

为了防止数据库无限增长，系统实现了自动化的过期数据清理机制。

### 5.1 策略概览

| 存储后端 | 清理策略 | 实现机制 | 频率 |
|----------|----------|----------|------|
| **Redis** | **TTL 自动清理** | 依赖 Redis 原生 `SETEX`/`EXPIRE` 机制，无需应用层干预。 | 实时 |
| **PostgreSQL**| **定期删除** | 由 `OAuth2CleanupService` 调用 `IGrantRepository` / `ITokenRepository` 的清理方法删除过期 Auth Code、Access/Refresh Token。 | 默认每 1 小时 |
| **Memory** | **定期扫描** | 同上，由 `OAuth2CleanupService` 触发各仓储的过期清理。 | 默认每 1 小时 |

### 5.2 调度器实现

清理由独立的 `OAuth2CleanupService`（`libs/drogon/src/plugin/OAuth2CleanupService.cc`）承担，在 `OAuth2Plugin::initAndStart` 中创建并启动，间隔由插件配置项 `cleanup_interval_seconds` 控制（默认 `3600`，见 `config.json`）：

```cpp
cleanupService_ = std::make_shared<OAuth2CleanupService>(grantRepo_, tokenRepo_);
double cleanupInterval = config.get("cleanup_interval_seconds", 3600.0).asDouble();
cleanupService_->start(cleanupInterval);
```

服务内部用 `drogon::app().getLoop()->runEvery(interval, ...)` 周期触发，并通过 `weak_from_this()` 防止在销毁后回调。

### 5.3 接口定义

清理不再集中于单一的 `IOAuth2Storage::deleteExpiredData`；而是按仓储拆分，由 `IGrantRepository`（Auth Code）与 `ITokenRepository`（Access/Refresh Token）各自提供过期删除方法，由 `OAuth2CleanupService` 编排调用。

## 6. 存储后端选型与 Memory 后端警告 (F-031)

> **⚠️ Memory 存储后端仅供测试 / 开发使用，生产环境禁用。**

`storage_type="memory"`（见 `config.ci.json`）将所有 client / token / code /
consent 数据保存在进程内存中，**密钥（client_secret）以明文存储**（不经
SHA-256 加盐哈希），且：

- 进程重启即丢失全部数据（无持久化）；
- 无多用户 / 多实例共享（每个进程一份独立状态）；
- 无事务、无原子 CAS 保证（测试桩实现）；
- Memory identity 仓库永远从 `findByUsername` 返回 `nullopt`，因此 admin
  登录链路在该模式下不可用（`loginAsAdmin()` 返回 `nullopt`，依赖它的集成
  测试会干净跳过）。

**生产部署必须使用 `storage_type="postgres"`**（Postgres 是唯一受支持的生产
存储后端；独立 Redis 存储模式已弃用，见 F-005 / [配置指南 §3](../operate/configuration-guide.md)）。
Memory 后端存在的唯一目的是让 Windows / macOS CI 环境在无 Postgres 时仍能
跑通不依赖 DB 的测试用例（contract 测试、纯单测、协议错误信封测试等）。

## 数据一致性专题

### 授权码单次使用（防双花）

`consumeAuthCode` 在存储层保证原子性：PostgreSQL 用 `UPDATE ... WHERE consumed = false ... RETURNING`
（raw SQL 豁免条款）；Redis 后端用 Lua 脚本；Memory 后端用互斥锁。契约由
`tests/contract/GrantRepositoryContractTest.cc` 的三实现同测覆盖。

### 缓存一致性：延迟双删

Redis L2 缓存（键前缀 `fulla:cache:`）的写路径失效采用**延迟双删**：立即 DEL + 事件循环上
延迟二次 DEL（默认 200ms，`cache.invalidation_double_delete_delay_ms` 可配，钳位 [50,2000]），
以覆盖"读线程在 DEL 前刚回填旧值"的竞态窗口（issue #79）。二次 DEL 失败可观测：
`fulla_cache_invalidation_failures_total{kind}` 计数器（issue #80）。读路径为 cache-aside，
未命中回源 PostgreSQL 后回填（TTL 兜底最终一致）。

### refresh token 家族与级联撤销

refresh token 存储家族标识（V008），检测到重放即撤销整个家族；撤销可按 token / client / user
三个粒度发起（管理 API 与 `/oauth2/revoke`）。
