# fulla 技术规范

> 本文档定义 fulla 项目的技术规范，包括架构设计、编码标准、安全要求等。
> （异步与 DB 操作规则已迁移至 `.claude/rules/`，本文件只保留引用指针——见 AGENTS.md 索引。）

---

## 一、架构规范

### [MUST] Drogon 框架优先原则
- 优先使用 Drogon 内置功能，避免引入三方库
- 引入新库必须在 PR 中说明必要性

### [MUST] 分层架构

| 层级 | 职责 | 关键要求 |
|------|------|----------|
| Controller 层 | HTTP 请求/响应 | 薄层设计，验证格式，调用 Plugin/Service |
| Plugin/Service 层 | 核心业务逻辑 | Plugin 模式，依赖注入，单例管理 |
| Storage 层 | 数据访问 | `IOAuth2Storage.h` 接口，Strategy 模式 |
| Model 层 | ORM 映射 | 禁止修改 ORM 类，用 `drogon_ctl` 重新生成 |

### 异步编程规范

> 完整规则见 [`.claude/rules/db-operations.md`](.claude/rules/db-operations.md)（权威源头），本节不重复。

要点：async callback 优先（`Mapper::findOne` / `execSqlAsync`）、`Mapper::findBy`-with-future 受限、`CoroMapper` 禁止；Callback 用 `std::make_shared<CallbackType>(std::move(cb))` 管理生命周期；每个 `Mapper<...>(dbClient)` 构造独立 try-catch。

**关于 `[this]` 捕获**：分层级 —— Domain 服务层禁止 `[this]`，须用 `shared_from_this()`；Controller 层允许 `[this]`（`HttpController` 是进程级单例，见 `TokenEndpointController.cc:1306`）。旧版本文件把 `[this]` 列为全局禁令，与 controller 层实践冲突，已移除该条。详见 `AGENTS.md`。

---

## 二、数据访问规范

> 完整规则见 [`.claude/rules/db-operations.md`](.claude/rules/db-operations.md)（权威源头），本节不重复。

要点：DB 操作必须 async callback + Mapper + Criteria 三件套；raw SQL 仅 6 种豁免（DDL / `UPDATE ... RETURNING` / 文档化批量 / `INSERT ... ON CONFLICT` / `SELECT 1` 探活 / 显式事务 `COMMIT`）；JOIN 禁用，拆查询或 `Criteria::In`；读写分离 `dbClientMaster_`（写）/ `dbClientReader_`（读），连接池配置在 `config.json`。

---

## 三、代码质量规范

### [MUST] 代码风格

| 规范项 | 要求 |
|--------|------|
| 语言标准 | C++17 |
| 风格指南 | Google C++ Style Guide (Drogon 默认) |
| 行长度限制 | 100 字符 |
| 格式化工具 | clang-format 自动格式化 |
| 字符规范 | 禁止 emoji，使用 ASCII 符号如 `[+]`, `[-]`, `[!]`（Windows 兼容性） |

### [MUST] 错误处理

| 错误类型 | 处理要求 |
|----------|----------|
| Drogon 异常 | 必须捕获: `catch (const DrogonDbException &e)` |
| 异步回调失败 | 必须在失败时调用 `(*sharedCb)(errorResult)` |
| 日志级别 | `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` |

### [MUST] 性能优化

| 优化项 | 要求 |
|--------|------|
| 接口选择 | 优先使用异步接口，避免阻塞 |
| 缓存策略 | 合理使用缓存 (CachedOAuth2Storage) |
| 数据库优化 | 使用索引，避免 N+1 查询 |
| 连接池配置 | 根据并发需求调整 |

---

## 四、安全规范

### [MUST] 输入验证

| 验证项 | 要求 |
|--------|------|
| 用户输入 | 所有用户输入必须验证 |
| SQL 查询 | 使用 ORM Criteria，禁止字符串拼接 |
| XSS 防护 | 使用 Drogon 内置 CSP 和模板转义 |

### [MUST] 认证授权

| 规范项 | 要求 |
|--------|------|
| OAuth2 流程 | 严格遵守 RFC 6749 |
| Token 有效期 | Access Token (1h), Refresh Token (30d) |
| 权限检查 | 所有受保护端点必须验证权限 |

### [MUST] 敏感数据保护

| 数据类型 | 保护要求 |
|----------|----------|
| 密码 | 使用 SHA-256 + salt 哈希 |
| Client Secret | 使用 SHA-256 + salt 存储 |
| 日志输出 | 禁止日志中输出敏感信息 (密码, token) |

---

**文档版本**: v2.1 | **最后更新**: 2026-09-27 | **维护者**: fulla 维护团队
