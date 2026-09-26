---
description: 改错误码/HTTP端点/迁移/公共头/OpenAPI 时必须完成的同步点硬清单（漏一处 CI 必挂）
globs:
  - "libs/common/src/error/**"
  - "libs/common/include/**"
  - "libs/drogon/src/controllers/**"
  - "libs/drogon/src/observability/**"
  - "apps/server/openapi.yaml"
  - "apps/server/migrations/**"
  - "apps/server/seed/**"
  - "tests/integration/error/golden/**"
  - "tools/api-diff/**"
---

# 同步点硬规则（CI 门禁对账清单）

细节、政策与本地预检命令见 `.claude/skills/ci-gate-sync/SKILL.md`；本文件是"动了就必须同步"
的最小硬清单。历史教训：同步点数量随时间**只增不减**（错误码从 4 处涨到 8 处），以本清单为准，
不要引用记忆里的旧计数。

## 新增/修改 Application 错误码（8 处）

1. `libs/common/src/error/ErrorCatalog.cc` 新条目 + **`std::array<RawEntry, N>` 尺寸字面量**
   必须同步改（漏改 = MSVC C2078 编译错误）
2. `ErrorCatalogRegressionTest` 的总数断言
3. `ErrorCatalogPropertyTest.cc` 的 `httpStatusOverrideFor()` 覆盖清单
4. `apps/server/openapi.yaml` 相关描述
5. `frontends/admin/src/services/messages/zh-CN.ts` + `frontends/user/.../zh-CN.ts`（双端）
6. `crossAppConsistency.property.test.ts` 的 code 域
7. `docs/domains/api-reference.md` §5 错误码目录表（`ErrorCatalogDocTest` 强制；改 §5 或做过
   繁简转换后必须重跑 `fulla-tests -r Unit_P0_ErrorCatalogDoc_*`）
8. `website/i18n/zh-CN/...` 镜像错误码表（文档站受影响时）

## 新增 HTTP 端点（5 处 + 2 条件处）

1. Controller 路由宏（`METHOD_LIST`）
2. `OpenApiGenerator::addEndpoint`（controller 的 `initApiDocs`）
3. `apps/server/openapi.yaml`（**operation 的 tags 块极易丢**，丢了 SDK drift 门挂）
4. `tests/.../Property4_OpenApiValidationBaselineTest.cc` 的 `kFingerprint` 冻结串
5. `tests/integration/error/golden/route_manifest.txt` golden 基线（更新法：删文件重跑自动重播种）
6. （条件）`scripts/backend/test-{oauth2,admin}-endpoints.{ps1,sh}`——若列表端点受分页影响，
   调用处补 `?q=` 搜索参数（分页后目标用户不一定在第 1 页）
7. （条件）`openapi.yaml` 任何改动后重生成 SDK：`python tools/clients/regen_clients.py`，
   并再生成 `apps/server/docs/api/openapi.json`（从 `apps/server` 为 CWD 短暂运行 server）。
   注意：openapi.json 是 C++ 文档注册的**派生产物**——只改 yaml 的响应文档/描述
   （不动路由、参数、schema）时它通常零 diff（该工件甚至不含 responses 表），
   复核无 diff 即为合规，无需强行制造变更。

## 新增 DB 迁移 / 新增公共头

- 新迁移：`python tools/migration-check/migration_check.py --update-baseline`；迁移必须幂等
  （db-reset 后服务端会整链重放）；基线内迁移**不可原地改**，写新的前向迁移。
- 新公共头（`libs/*/include/**`）：api-diff 基线批准（政策见 ci-gate-sync skill）。

## 本地预检（推送前）

```bash
bash .claude/hooks/local-gates.sh          # 一键：下列五门
python tools/openapi-governance/check_spec_governance.py   # spec 治理门
python tools/clients/regen_clients.py --check              # SDK drift
python tools/api-diff/api_diff.py                          # 头文件 SemVer
bash tools/test/scripts/naming_validator.sh                # 测试命名门
cmake --build build/windows-msvc --config Release          # 需 -DFULLA_WERROR=ON 配置过（见 ci-failure-triage）
```
