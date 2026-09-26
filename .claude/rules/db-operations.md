---
description: DB access must be the async + Mapper + Criteria combo; raw SQL only in 3 exemptions
globs:
  - "libs/**"
  - "apps/server/**"
---

Every database operation in this codebase MUST be the **async callback + Mapper
+ Criteria** combo. The three parts are jointly mandatory, not pick-one.

## The combo (all three, every query)

1. **Async callback** — prefer `Mapper::findOne` / `execSqlAsync` with a moved
   `std::function<...> &&callback` (last param). Synchronous
   `Mapper::findBy`-with-future is RESTRICTED, only when a sync result is truly
   needed. `CoroMapper` is FORBIDDEN. Never move one callback into two sibling
   lambdas of the same call (empty-function abort, A-2) — CI enforces it via
   `tools/arch-guard/double_move_guard.py` (#183).
2. **Mapper API** — SELECT via `Mapper::findBy` / `findOne`, INSERT via
   `Mapper::insert`, UPDATE via `Mapper::update`. No hand-rolled SQL for CRUD.
3. **Criteria** — build WHERE conditions with `Criteria`, never by concatenating
   strings into a query. For multi-row membership, use `Criteria::In(...)`. For
   compound conditions, chain `&&` / `||` on Criteria objects.

JOIN-in-a-single-query is forbidden — split into multiple queries (or
`Criteria::In`). Capture `auto self = shared_from_this()` in the callback to
avoid use-after-free.

## Mapper 构造异常防护

`Mapper<Model>(dbClient)` 构造可能因 `DbClient` 内部状态异常（连接断开、
之前操作失败导致连接处于不一致状态等）而抛出 `std::exception`。在异步
回调中未捕获会逃逸到 Drogon 事件循环并导致进程终止（SIGABRT/SIGSEGV）。

**要求 1：每个 `Mapper<...>(dbClient)` 构造语句所在的代码块 MUST 包裹在
`try { ... } catch (const std::exception &e) { ... } catch (...) { ... }`
中。** 不分位置——顶级代码、嵌套在 `findOne`/`findBy` 回调内部的
`Mapper` 构造各自需要独立的 `try-catch`，外层保护不到内层异步回调。

**要求 2：catch 块 MUST 调用 `(*sharedCb)(errorResult)` 将失败传递给上层
回调。** 禁止仅 `LOG_ERROR` 后 return（上层静默丢失数据）。
`errorResult` 根据回调类型选择：`StringListCallback → ({})`、
`AccessTokenCallback/RefreshTokenCallback → (std::nullopt)`、
`VoidCallback → ()`。

## The raw-SQL exemptions (and only these)

Raw SQL is allowed ONLY for:
- **DDL** (schema setup, migrations),
- **`UPDATE ... RETURNING`** (when you need the updated row back in one step),
- **documented batch operations** (state the justification in a comment),
- **`INSERT ... ON CONFLICT`** (upsert/reserve patterns the Mapper cannot express;
  justify in a comment),
- **connectivity probes** (`SELECT 1` health checks that touch no table),
- **explicit transaction `COMMIT`** (when durability MUST be confirmed before an
  external side effect — e.g. channel ACK / outbound call; an implicit
  destructor-time commit would race with the network call. Justify in a comment).

Anything else as raw SQL is a violation. A PreToolUse hook also guards
credential placeholders in these files, so failures show up before runtime.

The `project-conventions` skill holds the full statement; this file is the
path-scoped reminder that loads when you edit `libs/**` or
`apps/server/**`.
