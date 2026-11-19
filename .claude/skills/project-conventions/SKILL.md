---
name: project-conventions
description: Enforces OAuth2 project coding conventions from TECH_SPECS.md during code generation and review
user-invocable: false
---

# Project Conventions Checklist

Apply these rules to ALL C++ code generated or modified in this project. Loaded automatically by Claude as background knowledge.

## Architecture Rules

| Rule | Requirement |
|------|-------------|
| Layer separation | Controllers (HTTP) -> Plugin/Service (business) -> Storage (data) -> Model (ORM) |
| Drogon-first | Use Drogon built-ins over third-party libraries |
| Plugin pattern | Core logic in `libs/` (drogon, oauth2, storage-*), server wiring in `apps/server` |
| ORM immutable | NEVER edit files in `models/` -- use `drogon_ctl` to regenerate |

## Async Programming

| Pattern | Status |
|---------|--------|
| Async callbacks (`Mapper::findOne`, `execSqlAsync`) | REQUIRED -- always prefer |
| Synchronous (`Mapper::findBy` with future) | RESTRICTED -- only when necessary |
| Coroutines (`CoroMapper`) | FORBIDDEN -- never use |

### Lambda Capture Rules
- `[sharedCb]` -- REQUIRED for callback lifetime
- `[&var]` -- FORBIDDEN unless PR explains lifetime guarantee
- `[this]` -- **DOMAIN SERVICE LAYER FORBIDDEN** (no PR-exemption): in
  `libs/oauth2` / `libs/identity` services, never capture `[this]`; the class must
  `enable_shared_from_this<T>` and the lambda captures
  `auto self = shared_from_this()`, holding ownership so `this` stays alive for
  the whole async continuation.
- `[this]` -- **CONTROLLER LAYER ALLOWED**: in `libs/drogon/.../controllers`
  (Drogon `HttpController<T,false>` process-wide singletons, not
  `enable_shared_from_this`), `[this]` is permitted and common. Do NOT apply
  `shared_from_this()` there.

### Callback Pattern
```cpp
auto sharedCb = std::make_shared<std::function<void(const ResultType &)>>(
    std::move(callback));
// Use *sharedCb to invoke
```

## Data Access

| Operation | Allowed | Method |
|-----------|---------|--------|
| SELECT | ORM only | `Mapper::findBy`, `Mapper::findOne` |
| INSERT | ORM only | `Mapper::insert` |
| UPDATE | ORM only | `Mapper::update` |
| JOIN | Forbidden | Split into multiple queries or `Criteria::In` |
| Raw SQL | Exception only | DDL, `UPDATE...RETURNING`, batch ops |

## Error Handling

- Always catch `const DrogonDbException &e` for DB operations
- All async callbacks MUST handle failure path: `(*sharedCb)(errorResult)`
- Log levels: `LOG_DEBUG` (dev), `LOG_INFO` (flow), `LOG_WARN` (issues), `LOG_ERROR` (failures)
- NEVER log passwords, tokens, or secrets
- Always need try catch for all async callbacks

## Code Style

- C++17 standard, Google style, 100 char line limit
- clang-format runs automatically on edit (hook configured)
- ASCII only in code: use `[+]`, `[-]`, `[!]` instead of emoji
- No comments explaining WHAT -- name variables/functions to be self-documenting
- Comments only for WHY: hidden constraints, non-obvious invariants, workarounds

## Security

- Input validation on ALL user input
- ORM Criteria for queries (no string concatenation)
- Passwords: PBKDF2-SHA256, 310k iterations, random salt; client secrets/tokens: salted SHA-256 (`TokenCrypto::hashToken`)
- Token TTL: access 1h, refresh 30d
- PKCE required for all authorization_code clients (`auth.require_pkce`, default on)
- Rate limiting on login/token/password-reset endpoints

## Testing

- Framework: Google Test via Drogon (`drogon_test.h`)
- Coverage target: 80%+
- Handle both storage modes: `MemoryOAuth2Storage` and `PostgreSQL`
- Test naming: `^(Unit|Integration|E2E|Performance|Security|API|Database|Acceptance)_P[0-3]_<Module>_...` (naming_validator.sh enforces the P-priority segment)
