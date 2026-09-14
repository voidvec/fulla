# 测试策略与执行指南 (Testing Guide)

本文档说明项目的测试分层策略、各测试文件的覆盖范围，以及如何在本地执行全套测试。

---

## 1. 测试前置要求

在运行测试前，请确保以下服务已就绪：

| 服务 | 地址 | 说明 |
|---|---|---|
| **PostgreSQL** | `localhost:5432` | 数据库名: `fulla_db` / 用户: `fulla_user` / 密码: `123456` |
| **Redis** | `localhost:6379` | 密码: `123456`（与 `config.json` 一致）|

> **快速启动基础设施**：如果你使用 Docker，可以单独启动 postgres 和 redis 容器:
> ```powershell
> docker run -d -p 5432:5432 -e POSTGRES_USER=fulla_user -e POSTGRES_PASSWORD=123456 -e POSTGRES_DB=fulla_db postgres:17-alpine
> docker run -d -p 6379:6379 redis:7-alpine redis-server --requirepass 123456
> ```

---

## 2. 测试分层

测试编译为**两类可执行文件**：

1. **按库的 gtest 二进制文件**（Domain 层，纯单元测试，无 DB/无 Drogon）：
   - `libs/common/test/fulla-common-test` — `ConfigManager`、`ErrorCatalog`、`Result`、值对象
   - `libs/common/testing/test/fulla-common-testing-test` — 假实现（`FakeClock`/`FakeCryptoProvider`/`FakeLogger` 等）的确定性验证
   - `libs/oauth2/test/fulla-oauth2-test` — `TokenService`/`AuthorizationService`/`ClientService`/`JwkManager`/`Pkce`/`ScopeDecisionEngine`/`TokenCrypto`
   - `libs/identity/test/fulla-identity-test` — `AuthService`/`MfaService`/`TotpUtils`/`SessionManager`/`WebAuthnService`/社交登录（Google/WeChat/GitHub）
   - 这些用 gtest（非 `DROGON_TEST`），由各 lib 的 `test/CMakeLists.txt` 通过 `gtest_discover_tests` 注册为独立 ctest 条目。

2. **主测试二进制文件 `tests/fulla-tests`**（`DROGON_TEST` 框架，包含所有需要 Drogon/DB 的层级）：

| 层级 | 目录 | 覆盖范围 | 外部依赖 |
|---|---|---|---|
| **Level 1 — 单元测试** | `tests/unit/`（`config/`、`error/`、`utils/`、`validation/`、`plugin/`、`schema/`、`subject/`、`initorder/`） | 纯逻辑：错误信封、密码哈希、PKCE/CryptoUtils、RuleSet 校验、配置加载、OpenAPI 生成 | 无 |
| **Level 2 — 契约测试** | `tests/contract/` | 跨后端（Postgres/Redis/Memory）的仓储契约一致性：`IClientRepository`/`IGrantRepository`/`ITokenRepository`/`IConsentRepository`/`IUserRepository` | Memory 必跑；Postgres/Redis 在 `getPostgresClientOrNull()`/`getRedisClientOrNull()` 返回空时自动 skip |
| **Level 3 — 集成测试** | `tests/integration/`（`auth/`、`token/`、`storage/`、`concurrency/`、`error/`、`plugin/`） | 完整业务流程、并发竞态、错误信封、插件组装 | Postgres / Redis（memory-only 模式 `-DFULLA_MEMORY_TESTS_ONLY=ON` 下跑 Memory 子集） |
| **Level 4 — 安全测试** | `tests/security/` | SQL 注入、XSS、命令注入、CORS、Token 安全、速率限制 | Postgres / Redis |
| **Level 5 — E2E/功能** | `tests/e2e-backend/`、`tests/performance/` | OAuth2 完整流程、性能基准 | Postgres + Redis + Drogon App |

> 内存模式：配置 `-DFULLA_MEMORY_TESTS_ONLY=ON` 可在**无外部 DB** 时跑完整套件（Postgres/Redis 测试自动 skip）——这是 Windows CI 的做法。

> 安全测试用例数、功能测试用例数与覆盖清单见各目录下的测试文件头注释；本节不再硬编码具体数量（数量随迭代增长，统一以 §7 的实测统计为准）。

### DROGON_TEST 断言书写规范 — `CHECK`/`REQUIRE` 内禁止裸布尔运算符 [#MUST]

drogon 的 `CHECK`/`REQUIRE` 是**宏不是函数**：`CHECK_INTERNAL__` 把表达式展开为
`(drogon::test::internal::Decomposer() <= expr)`，而宏实参替换不加括号，所以**裸写的
`a || b`（或 `a && b`）会重结合成 `(Decomposer() <= a) || b`**——结果静默错误，症状像
"断言随机挂"。真实案例（PR #68 调试）：`CHECK(body.isMember("error") || body.isMember("code"))`
失败，但 `LOG_INFO` 打出的原始 body 里明明有 `error` 键。

**规则**：`CHECK(...)`/`REQUIRE(...)` 的顶层实参里出现 `||`/`&&` 时，必须满足其一：

```cpp
// ✅ 拆成两条断言（首选——失败信息更精确）
CHECK(body.isMember("error"));
CHECK(body.isMember("code"));

// ✅ 整体加括号（外层括号让链式表达式作为一个操作数绑定到 <=）
CHECK((a != std::string::npos || b != std::string::npos));

// ✅ 仓库既有先例：显式 (bool) 转换（tests/e2e-backend/oauth2_flows/FunctionalTest.cc）
CHECK((bool)(response.find("code=") != std::string::npos ||
             response.find("error") != std::string::npos));

// ❌ 禁止：裸顶层布尔链（宏展开后语义被破坏）
CHECK(body.isMember("error") || body.isMember("code"));
```

说明：运算符嵌套在**调用/下标/子表达式括号内**（如 `CHECK(f(a || b))`、`CHECK(x == (a || b))`）
不受影响——它在绑定给 `<=` 之前已求值。`CHECK_THROWS`/`REQUIRE_THROWS` 系列走 `EVAL__`
路径，也不受影响。

**CI 强制**：`tools/test/scripts/drogon_macro_bool_check.py` 扫描 `tests/` 树并对违规
报错（static-checks 步骤，与命名规范检查并列）；`--selftest` 可自验。

### Level 4 补充明细 — 安全测试 (Security Tests)

| 测试文件 | 覆盖范围 |
|---|---|
| `SecurityTest.cc` | SQL 注入、XSS、命令注入、输入验证、CORS、Token 安全、速率限制、健康检查安全 |

覆盖要点：输入验证（注入/长度/空值）、认证授权（无效凭据、速率限制）、CORS 双向、敏感数据传递、Token 安全（无效/缺失授权码与 Refresh Token）、安全头（含 HSTS）、暴力破解防护、健康检查信息泄露。

### Level 5 补充明细 — E2E / 功能测试 (Functional Tests)

| 测试文件 | 覆盖范围 | 依赖 |
|---|---|---|
| `IntegrationE2ETest.cc` | 模拟完整 OAuth2 授权码流程：HTTP 请求 → 授权 → 登录 → 换 Token → UserInfo 验证 | Postgres + Redis + 运行中的 Drogon App |
| `FunctionalTest.cc` | OAuth2 完整流程、错误处理、UTF-8/Emoji 字符、健康检查、RBAC、Token 生命周期、输入验证、速率限制 | Postgres + Redis |

覆盖要点：完整授权码流程、错误场景、UTF-8/Emoji 边界、RBAC 未授权路径、Token 生命周期异常路径、超长输入、速率限制检测、端点可用性。

> 用例数量随版本演进，**以 `ctest -N` 实测为准**（当前全套基线见 §7）。

---

## 3. 执行方式

### 方式一：通过 CTest（推荐）

```powershell
# 在构建完成后执行（目录为 build/<preset>，Windows Release 为 windows-msvc）
cd build\windows-msvc
ctest -C Release --output-on-failure --timeout 120
```

> **输出策略**：默认仅打印失败用例的日志与末尾汇总（`成功数 / 失败数 / 总数`）。如需查看每个用例（含通过用例）的完整输出，加 `--verbose`（`-V`）；如需完全静默、只看汇总行，加 `-Q`。`manage.sh test-backend -q` / `manage.ps1 test-backend -q` 等价于 `-Q`。

### 方式二：直接运行测试可执行文件

测试可执行文件内部会自动启动 Drogon App 实例（`test_main.cc` 中通过信号量同步），**无需手动启动后端服务**。

```powershell
cd build\windows-msvc\tests\Release
.\fulla-tests.exe
```

### 方式三：使用 manage 脚本

`manage.ps1`（Windows）/ `manage.sh`（Linux/macOS）封装了与 CI 相同的构建+测试流程：

```powershell
# 构建并运行后端测试套件（与 manage.sh test-backend 等价）
.\manage.ps1 test-backend

# 完整循环：构建 + 单元/集成测试 + 管理端点 API 测试
.\manage.ps1 full-test

# 仅跑管理端点 / OAuth2 端点的 API 脚本
.\manage.ps1 test-admin-endpoints
.\manage.ps1 test-oauth2-endpoints
```

### full-test 流水线的三层执行关系（以及去重了什么）

`manage full-test`（由 `scripts/backend/full_test.bat` / `full-test.sh` /
`full-test-docker.sh` 驱动）叠加了三层。理解它们的重叠关系，才能判断一次
全量到底实际执行了什么：

1. **ctest 层** —— `EndpointTests_OutOfProcess`（label `Endpoint`）是一条
   普通 ctest 条目：它自己起服、对它跑 59 条 OAuth2 + 52 条管理端点脚本、
   然后停服（见 `tests/CMakeLists.txt`）。当其运行环境（服务二进制 /
   shell）不可用时，以 `SKIP_RETURN_CODE=77` 报告跳过。
2. **双配置层** —— `test.bat` / `test.sh` 会把**整套** ctest 套件跑
   **两遍**：一遍标准 `config.json`（PostgreSQL），一遍 `config.ci.json`
   （memory 存储）。这层重复是有意设计 —— 两种存储后端通过同一套件是
   发布信心的来源。
3. **手动端点层** —— 流水线自己的“起服 → 跑端点脚本 → 停服”步骤。由于
   第 1 层已经在同样的标准配置下跑过同一套脚本，当本次 ctest 运行的
   JUnit 报告（`build/<preset>/Testing/junit-config-standard.xml`，由
   `test.bat`/`test.sh` 写出）证明 `EndpointTests_OutOfProcess` 本次实际
   跑绿时，这一层会自动跳过（#119）。任何不确定情形 —— 报告缺失、条目
   缺失、被跳过、无法解析 —— 手动层照旧执行，因此 ctest 条目退出（77）
   的环境仍保有端点覆盖路径。

同理适用于逐条注册的 `Contract.*` ctest 条目：每一条也会随 `OAuth2Tests`
二进制整体执行而跑到。它们单独注册成 ctest 条目只是为了提供标签化入口
（`ctest -L Contract`）；在上述意义上不会构成对同一用例的第二遍执行。

---

## 4. 测试输出示例

```
All tests passed (N assertions in M tests)
```

如果出现失败，失败的测试名称和断言位置会被打印：
```
In test case SomeTestName
  SomeTestFile.cc:63  FAILED:
    CHECK(c.has_optional())
```

**常见失败原因**：
- Redis 或 PostgreSQL 服务未启动 → 检查服务是否可达
- Redis 密码不匹配 → 检查 `config.json` 中的 `passwd` 字段
- 数据库未初始化 → 执行 `apps/server/migrations/` 目录下的迁移脚本（后端在 `FULLA_AUTO_MIGRATE=true` 时也会自动执行）

---

## 5. CI 中的测试

每次 Push 到 `master` 或发起 PR 时，GitHub Actions CI 会自动执行：

1. 启动 Postgres 和 Redis Service Container
2. 初始化数据库 Schema
3. 编译项目
4. 运行 `ctest`

详见 [CI/CD 指南](../contribute/ci-cd-guide)。

---

## 6. 测试报告 (Test Reports)

历史的安全/功能测试报告、Bug 状态分析与连接泄漏验证报告属于过程性档案，已随文档治理移出仓库（维护者本地保存）。当前测试状态以 CI 与本节口径为准：

- **CI 全绿**是合入门槛（三平台矩阵，见 [CI/CD 指南](ci-cd-guide.md)）。
- 安全与功能覆盖面见 §2 Level 4/5 明细；数量以 `ctest -N` 实测为准。
- 历史报告中的结论性内容（如 April 快照中发现的安全缺陷）已修复并沉淀为回归测试用例。

---

## 7. 测试覆盖率总结

### 总体测试状态

> 以下数字为**实测统计**（Windows MSVC Release 构建，无外部 DB，Postgres/Redis 测试 skip）。在带 Postgres+Redis 的环境（如 Linux CI 或本地 WSL+Docker）下，被 skip 的契约/集成测试会激活，ctest 条目数会进一步增加。

| 测试来源 | 通过 | 失败 | 总计 | 通过率 |
|---------|------|------|------|--------|
| **按库 gtest 二进制**（2026-06 基线快照；当前全套 ctest 为 501，以 `ctest -N` 实测为准） | 364 | 0 | 364 | 100% |
| **主测试二进制 ctest 条目**（含 Contract 标签 + OAuth2Tests 全量） | 450 | 0 | 450 | 100% |

> 注：两列数字有重叠关系——主二进制内含所有 `DROGON_TEST` 单元/集成测试（作为一个 `OAuth2Tests` 条目运行）；按库 gtest 二进制是 Domain 层的纯单元测试，独立编译运行。`450` 条 ctest 中 84 条带 `Contract` 标签（可用 `ctest -L Contract` 单独跑）。

### 代码覆盖率

实测行覆盖率（gcov，gcc 13.3 Debug 构建，WSL Ubuntu 24.04，Postgres+Redis 激活；排除 ORM 自动生成的 `models/`；测量于 7ba8068，全部 5 个测试二进制均已执行）：

| 库 | 行覆盖 | 备注 |
|---|---|---|
| libs/common | 69.4% (318/458) | ErrorCatalog/ErrorTypes/ErrorContext/ConfigManager（由 per-lib gtest 二进制 `fulla-common-test` 驱动）；ConfigManager 环境相关分支与 ErrorCatalog 部分分支未覆盖 |
| libs/identity | **96.9%** (590/609) | Auth/Mfa/WebAuthn/Social/Totp/Session |
| libs/storage-memory | **97.1%** (431/444) | Memory 后端全方法覆盖（CI 必跑路径） |
| libs/oauth2 | **92.1%** (627/681) | TokenService/AuthService/ClientService/JwkManager/Pkce |
| libs/storage-redis | 46.2% (306/663) | 契约测试覆盖 getClient/validate/grant/token/consent 主路径；Lua 脚本与 transaction CRUD 待补 |
| libs/storage-postgres | 43.9% (727/1657) | 契约测试覆盖主路径；剩余盲区为事务/错误回退分支（需注入故障才能触发） |
| libs/drogon | **53.5%** (4807/8978) | admin 0%→55-69%、admin 控制器 0%→91-100%；authorize/health/discovery/mfa/deviceauth/userselfservice/apidoc 控制器补强；社交 OAuth 控制器经 mock 注入补强（Google 38.3%、WeChat 30.6%、GitHub 32.5%）；WebAuthn 39.2%（非加密 stub，无需 authenticator） |
| **整体** | **57.9%** (7806/13490) | 上表逐库求和（`scripts/measure_coverage.py` 的 OVERALL 即逐库之和）；从 48.5% 基线提升 +9.4pp |

> 上一轮基线为 48.5% (7091/14631)；本轮通过 admin 层 HTTP 集成测试 + 控制器补强 + 社交 OAuth/WebAuthn 的 mock 注入测试（`tests/common/SocialMockFixture.h` + `libs/identity/include/fulla/identity/testing/` 的共享 Fake）将整体提升到 57.9%。社交 OAuth 的 Google/WeChat/GitHub 均可经 mock 注入在 memory 模式下跑（注入路径不写 DB）；其中 GitHub happy-path 最初因 `issueTokensForUser` 直连 `getDbClient()` 无法在 memory 模式覆盖，随后已重构为经 `OAuth2Plugin::saveTokenPair` 存储抽象持久化（见 `SocialLoginHttpTest.cc` 的 `Integration_P0_GitHubLogin_FakeExchange_ReturnsTokens`），happy-path 现已可测（GitHubController 从 5.9% 提升到 32.5%）；WebAuthn 为非加密 stub，Postgres 模式下完整可测。注：此前文档的 58.8% 系旧逐库数字求和（其中 common 的 98.8% 为陈旧数据，现 `libs/common/src` 仅 4 个源文件 458 行，实测 69.4%）；本表已全部替换为 7ba8068 的实测值。剩余盲区：storage-postgres 事务/错误回退分支（需故障注入）。

#### ⚠ 实测覆盖率必须跑全部 5 个测试二进制

实测数字依赖**全部 5 个测试二进制**都执行（仅跑主二进制 `fulla-tests` 会漏掉 4 个 per-lib gtest 二进制贡献的 domain 层覆盖率，common 会被低估到 ~60%）：

1. `libs/common/test/fulla-common-test`（40 用例）
2. `libs/common/testing/test/fulla-common-testing-test`（43 用例）
3. `libs/identity/test/fulla-identity-test`（130 用例）
4. `libs/oauth2/test/fulla-oauth2-test`（151 用例）
5. `tests/fulla-tests`（450 条 ctest，含所有 `DROGON_TEST` 单元/集成/契约/admin HTTP 测试）

在 coverage 构建目录下依次运行这 5 个二进制后再聚合 `.gcda`。

#### ⚠ gcovr 路径匹配 bug —— 用 `scripts/measure_coverage.py` 代替

`gcovr 8.6` 对部分文件（如 `ClientManagementService.cc`）会误报 **0%**：raw `gcov` 明确显示 `Lines executed:55.79% of 328`，但 gcovr 的 `--print-summary` 只列出文件名不带百分比（gcovr 的源路径匹配对含绝对路径 + Drogon 头文件的 `.gcov` 输出处理不一致）。这是 gcovr 的已知路径匹配问题，不是零计数 bug（gcov flush 工作正常，见下）。

**可靠的聚合方式**：`scripts/measure_coverage.py` 直接用 `gcov -j`（JSON 格式，每文件 `{file, lines[{count, unexecuted_block}]}`）聚合，绕过 gcovr 的文本路径匹配。用法：

```bash
cd <repo>
# 先跑全部 5 个二进制（见上一节），再生成 JSON + 聚合：
find build/linux-coverage/libs -path "*/src/*" -name "*.gcda" ! -path "*/models/*" \
  | xargs -I{} bash -c 'cd "$(dirname {})" && gcov -j "$(basename {})" >/dev/null 2>&1'
find build/linux-coverage/libs -path "*/src/*" -name "*.gcov.json.gz" ! -path "*/models/*" \
  | python3 scripts/measure_coverage.py
```

gcovr 仍可用于生成逐文件 HTML 报告（`--html-details`），但**汇总百分比以 `scripts/measure_coverage.py` 为准**。

#### 覆盖率工具链

- `cmake/Coverage.cmake`（`oauth2_apply_gcov(target)`）给每个 first-party 库 + 测试可执行文件加 `-fprofile-arcs -ftest-coverage`，并显式链接 libgcov（仅 GCC；Clang 的 profile 运行时由 `-fprofile-arcs` 链接选项自动提供，无 libgcov）。
- `tests/test_main.cc` 在两处 `std::_Exit()` 前显式调用 `__gcov_dump()`：因为 `_Exit` 绕过 `atexit`，libgcov 的计数器 flush 不会自动执行（否则 gcov 读到全 0 计数）。这是 Drogon 测试框架 fast-exit 与 gcov 的已知交互，需在测试 main 里手动补 flush。（注：4 个 per-lib gtest 二进制正常退出，不走 `_Exit`，因此它们不需要手动 flush。）
- 覆盖率当前阶段性目标：60%（7ba8068 实测 57.9%）。剩余盲区集中在难以 HTTP 测试的分支：WebAuthn 典礼/加密深分支、UserSelfService（需逆向 auth 前置 filter）、storage-postgres 事务/错误回退分支（需故障注入）。社交 OAuth 控制器已可通过 `SocialMockFixture.h` 的 Fake 注入在 memory 模式下覆盖（GitHub happy-path 经 `saveTokenPair` 存储抽象重构后不再依赖 `getDbClient()`）。ORM 自动生成的 `libs/storage-postgres/src/models/*.cc` 已从分母排除。

---

## 8. 手动验证与 API 测试 (Manual Validation)

除了自动化测试套件外，项目还提供了用于手动验证端点功能的工具和脚本。

### 8.1 PowerShell 自动化验证脚本
项目提供了完整的 OAuth2 端点测试脚本：`scripts/backend/test-oauth2-endpoints.ps1`。

**使用方法：**
```powershell
# 临时绕过执行策略运行测试
powershell -ExecutionPolicy Bypass -File scripts/backend/test-oauth2-endpoints.ps1
```
该脚本会依次执行健康检查、登录、授权码交换、UserInfo 访问及管理员面板验证。

### 8.2 多环境 API 测试 (curl)
不同的命令行工具对 curl 语法的支持不同：

*   **PowerShell (推荐)**: 使用 `Invoke-RestMethod`。
*   **Git Bash**: 支持标准的 Unix 单引号语法。
*   **CMD**: 需要使用双引号并转义 `&` 符号为 `^&`。

**示例：登录并获取 JSON 响应**
```bash
# Git Bash 示例
curl -X POST http://127.0.0.1:5555/oauth2/login \
  -d 'username=admin&password=admin&client_id=fulla-portal&redirect_uri=http://localhost:5173/callback&json=true'
```

---

## 9. 故障排查 (Troubleshooting)

### 9.1 常见问题与对策
*   **服务器无法启动**：检查端口 5555 是否被占用 (`netstat -ano | findstr :5555`)，并确保 `config.json` 路径正确。
*   **登录失败 (400)**：确认用户名密码匹配，且数据库中存在该用户。检查 `redirect_uri` 是否与配置完全一致。
*   **Token 交换失败**：授权码 (Code) 仅能使用一次且有有效期。确保 `client_id` 和 `client_secret` 正确。
*   **PowerShell 脚本限制**：如提示“禁止运行脚本”，请使用 `-ExecutionPolicy Bypass` 参数。

### 9.2 调试技巧
*   **日志级别**：排错时可在 `config.json` 中临时将 `log_level` 调为 `DEBUG`（甚至 `TRACE`）以获取详细输出，定位完成后调回 `INFO`。完整的六级语义与约定见 [observability.md §3.2](../operate/observability.md)。
*   **实时日志**：使用 `Get-Content apps/server/logs/drogon.log -Wait -Tail 20` 监控运行状态。

---

**相关文档**:
- [Security Architecture](../architecture/security-architecture.md) - 安全加固与安全架构设计
- [Data Consistency](../architecture/data-persistence.md) - 数据一致性和威胁模型
- [API Reference](../domains/api-reference.md) - API 接口文档
