# 文档治理设计 v4 — 基于全文深读的内容裁决 · 双语 Docusaurus 站点

> 状态：v4（2026-08-27）。v1 目录级分类 → v2 四路全文深读的内容级裁决（file:line 证据）
> → v3 落地终稿（IA 三层模型、Phase A/B 完成、上站前质检）→ **v4：英文主站 +
> zh-CN locale 双语切换**（docs/ 为英文正本，中文树在 website/i18n/zh-CN/），
> Phase C 三篇深潜完成。
> 原则：**入库是给别人看的（stranger 能据此完成一件事）；留在本地是给维护者和 agent 看的。**

## 一、入库判据（不变）

1. **可执行**：stranger 依此完成一件事（部署/集成/排查/贡献）；
2. **可决策**：记录理解系统所必需的设计决策及理由（ADR）；
3. **可信任**：对外承诺（安全策略、合规尽调、版本政策、测量报告与方法论）。

## 二、逐篇内容裁决（136 文件 + 根部文档）

### 2.1 docs/backend/（23 篇 md + swagger-ui 静态资源）

| 文件 | 裁决 | 关键内容证据 |
|---|---|---|
| architecture-overview.md | **SITE-READY** | 章节事实全部与现状吻合；建议补 identity 包与缓存装饰器一笔 |
| ci-cd-guide.md | **SITE-READY** | 与 8 个实际 workflow 核对无硬伤；补 clients-sdk/security 两条一句话 |
| docker-deployment.md | **SITE-READY** | 与 compose 实测全吻合；池参数按 bench 结论（64）更新 |
| sdk-integration-guide.md | **SITE-READY** | 仅 L8 `.kiro` 来源路径失效需修 |
| sdk-runtime-contract.md | **SITE-READY** | 仅 L7 `.kiro` 路径失效需修 |
| versioning-and-release.md | **SITE-READY** | 治理单一出处；L294 "840 commits" 待办被版本重置超越需改写 |
| api-reference.md | **REWRITE** | L226 end_session"不验签"已被 #78 推翻（同文错误表却收录 4006，自相矛盾）；L254 Google 路由错；§6 教旧 openapi.json 工作流与治理门冲突 |
| configuration-guide.md | **REWRITE** | L38 PG15→17；L69 缓存层写成 "future"（已上线且 config.json:152 有配置块）；L132 end_session 旧语义；L176 JWKS 路径错；环境变量表仅 5 个远不全 |
| data-persistence.md | **REWRITE** | §3 与 §6 对 Redis 存储是否弃用自相矛盾（L96 vs L205）；schema 停在 V002（现 V026）；`fulla:cache:` 键空间缺席；吸收 data-consistency 后补延迟双删一节 |
| observability.md | **REWRITE** | 缺 #80 缓存失效指标；`oauth2_*` vs `fulla_*` 指标命名口径未澄清；审计示例 2026-01 陈旧 |
| oidc-guide.md | **REWRITE** | L21/34 JWKS 路由错（实为 `/.well-known/jwks.json`，照抄会拿不到密钥）；缺 end_session/backchannel 集成义务、auth_time/acr/amr、官方 SDK 通道 |
| rbac-guide.md | **REWRITE** | L62 "roles 进 JWT 未来支持"已实现（TokenService.cc:334）；完全缺 scope 层（V023 双闸模型）；手工 SQL 授予过时（有 admin API） |
| security-architecture.md | **REWRITE** | L36 secret 传输说法违反 F-017（默认 Basic 头）；威胁表未覆盖 PR#85 已修威胁（#78 伪造登出/#79 缓存竞态/#54 软删除绕过） |
| testing-guide.md | **REWRITE** | L274 测试数 364+450（实为 501）；L86-119 四月快照；L195-263 四份"[DOC]（已归档）"悬空引用 |
| data-consistency.md | **MERGE → data-persistence** | 窄而正确，但缺 #79 延迟双删这一最新一致性内容 |
| docker-guide.md | **MERGE → docker-deployment** | 与后者六成重叠；L19 容器命名 `oauth2-{service}` 漏改；保留其独有：命名规范表/调试容器/full_test_docker 脚本 |
| google-guide.md + wechat-guide.md | **MERGE → social-login.md** | 同构孪生篇；google L45 无按钮 vs L53 点按钮自相矛盾；以 GitHub 已接线为主线索 |
| plugin-integration.md | **MERGE → sdk-integration-guide** | 后者 §3 的 quickstart 子集 |
| security-hardening.md | **MERGE → security-architecture** | 限流数字与 config.prod.json 全面不符（3/2/5 vs 文档 5/5/10）；四月快照与悬空引用删除；须注明 Hodor 仅 prod 启用 |
| database-encoding-guide.md | **LOCAL** | 单机 SQL_ASCII 排查记录；含危险的 pg catalog DELETE 建议 |
| documentation-standards.md | **LOCAL** | 仓库目录元规则，随 CONTRIBUTING 走；本治理落地时改写 |
| ddd-domain-model.md | **ARCHIVE** | 自称"未经评审提案"；现状映射诚实，是未来演进底稿非现状文档 |

### 2.2 docs/ops/ + admin/ + frontend/ + performance-optimization/（16 篇）

| 文件 | 裁决 | 关键证据 |
|---|---|---|
| ops/account-lockout.md | **SITE-READY** | 扎实；凭证口径需统一（见矛盾 #1） |
| ops/postgresql-major-upgrade.md | **SITE-READY** | 最新且准确；修 L122 deploy 名；**补入 docs/README 索引（现在漏挂）** |
| ops/deployment.md | **REWRITE（小）** | L571-583 initdb.d 手动迁移在 prod 不可执行（迁移已烘焙进镜像）；L783 Prometheus 直连与 loopback 绑定矛盾；性能调优节已正确同步最新结论 |
| ops/deployment-windows-docker-desktop.md | **REWRITE** | 测试数 55/51 过期（实 59/52）；"80% 通过即成功"坏口径；6 处本机路径；admin123 凭证冲突 |
| ops/verification-checklist.md | **REWRITE** | dev 容器表混入不存在的 nginx；`oauth2_migrations` 表名错（实 schema_migrations）；硬编码密码 WinDockerTest2024!；表数量门槛 >=7 过期；同文两个 admin 邮箱 |
| ops/security-checklist.md | **MERGE → backend/security-hardening** | 整改结项备忘；L76-84 两条 filter-branch 命令复制粘贴事故 |
| admin/e2e-testing-guide.md | **REWRITE（轻）** | L903 死链；附录"7 文件/53 用例"实为 16/174；§9 与 account-lockout 全篇重复 |
| admin/test-cases.md | **SITE-READY** | 无硬伤，与实际 spec 对应健康 |
| frontend/test-cases.md | **SITE-READY** | 无硬伤，覆盖当前功能面 |
| performance-optimization/ 全部 7 篇 | **LOCAL** | prompt 是 AI 会话产物；wave 报告/仪器化/非代码方案/内存调查全是内部证据链（基线代际混乱，混排上站会呈现三个互相矛盾的 QPS 世界观）；面向用户的结论已正确抽取到 ops/deployment §性能调优；upstream-drogon-session-issue.md 是上游约束唯一记录，issue 发出后转链接再 ARCHIVE |

### 2.3 docs/history/（60 篇）：ADR 矿

**修正 v1 误判**：superpowers/specs/ 并非全部会话产物——其中 2 篇是真设计文档。

- **ADR 转化清单（11 + 1 备选，按优先级）**——每篇已提炼决策陈述：
  1. **产品 + 双 SDK 架构与依赖铁律**（sdk-refactor §2/§4.1/§5.2：Domain 禁 Drogon、oauth2/identity 互不依赖、端口下沉 common、arch-guard 强制）
  2. **Drogon 自注册符号链接策略**（sdk-refactor §5.5/§5.7：显式 registerController 替代 whole-archive + 插件零改动方案 A）
  3. **ErrorCatalog 单一权威与双通道错误**（error-code AD-1..6 + auth-flow-gap "不折叠原则"与 G7 防枚举例外）
  4. **Opaque Access Token + 凭据哈希存储 + 迁移不可变**（production_hardening_spec §五；**注意**：决策表写 Argon2id 实际落地 PBKDF2-SHA256 310K，转 ADR 时必须修正）
  5. **email 为主登录标识**（email-first §7 五项决策，V020 在库）
  6. **异步回调生命周期模式**（并发审计四主线；CacheMap 线程安全语义结论）
  7. **MFA 第二因子会话绑定**（mfa-fix：pending 绑定 + 防枚举同码，V022 在库）
  8. **首方 SPA 登录凭证暴露面控制**（mfa_auth_code_pkce §6 修正版 + 现状 authService.ts：AJAX+PKCE 闭包、token 不落 localStorage、托管登录页方案搁置）
  9. **ORM 生成模型豁免 + 迁移冻结**（repo-refactor §0/§1.2——已穿越两次重构验证）
  10. **限流选型 Hodor**（superpowers/specs：令牌桶三级限流，config.prod 在用）
  11. **集成测试平台分档**（http-integration-plan：DB-backed 测试限 Linux、进程内起 app、社交面不可达结论）
  12. （备选）**客户端认证 PUBLIC/CONFIDENTIAL 分类**（client-secret spec）
  另：productization/done/async-refactor-assessment 蒸馏"**为何 C++17 禁协程**"一页 ADR（贡献者必问）。
- **ARCHIVE 保留 11 篇**（history/README + 各 bugfix/审计原始记录 + 6 篇 superseded 设计）；
- **LOCAL 37 篇**（全部 tasks/requirements/plans 类 checkbox 文档与过程稿）；
- **DELETE 1 篇**：PRD/frontend_design.md（是 frontend/oauth2_frontend_design.md 的严格子集+更早快照，已 diff 确认）。

### 2.4 productization-evolution/ + branding/（25 篇）：两处对 v1 的硬修正

- **LOCAL 21 篇**（含 content-strategy "去 AI 味+软文流程"——公开即自伤；progress-status 枚举未修复安全项 #71/#73——不进站放大；research 含未公开定价 $499/$5000）；
- **例外一（SITE，benchmark 区）：in-progress/competitor-benchmark-design.md 必须保持入库**——README 徽章链的 COMPARISON.md 与双语 README、benchmarks/competitors/README 共**三处公开入口指向它**；内容为可公开方法论（三同原则/官方配置出处/诚实修订记录），环境披露（WSL2 8vCPU/16GB）是复现性必需且已在公开 README 中；出库即断三条公开链。迁移：为它脱离即将出库的目录单独安家（建议挪至 `docs/benchmark/`）；
- **例外二（SITE，档案/信任区）：done/oauth-oidc-compliance-audit.md 保持入库**——31 项全部已修复的 RFC 合规尽调，CHANGELOG.md:453 公开引用；是评估者眼中的信任资产（加"2026-08-07 基线快照"标注）；
- **branding/rename-impact-fulla.md → ARCHIVE 入库但先脱敏**：删除/泛化 §L9 本机路径与工作区细节（含完整磁盘路径、记忆库哈希、WSL 路径）；压缩 P0 "先占资产"步骤（暴露 fulla.dev/PyPI 占位意图）；CHANGELOG:40 引用其 §3，出库断链；
- **branding/repo-professionalization-audit.md → ARCHIVE 入库**：AGENTS.md 公开引用其为入库标准；
- **branding/rename-candidates.md → LOCAL（最高敏感级）**：暴露 fulla.dev 未注册状态 + 9 个备选名可用性清单 + 自贬评估——资产落袋前等于给抢注者递情报；
- 卫生项：`.mimosa/` 会话 JSON 已混入 docs/productization-evolution/（未跟踪）→ .gitignore 增补 `.mimosa/`（已有？核实）。

## 三、跨文档矛盾登记簿（Phase A 必修清单）

深读发现的矛盾**必须在上站前修平**，否则站点会把矛盾双语放大：

| # | 矛盾 | 定谳依据 |
|---|---|---|
| 1 | **admin 默认凭证三种口径**（'admin' vs admin123 vs admin/admin123+fulla-admin-console 双客户端） | 以 apps/server/seed/dev_admin_user.sql 实测定谳，全站统一 |
| 2 | end_session "不验签"（api-reference L226、configuration-guide L132）vs 错误表收录 4006 | 代码已强制验签（#78）；两处正文改写 |
| 3 | Redis 缓存层 "future"（configuration-guide L69）vs 已上线（architecture-overview L12 还指错章节） | config.json cache 块为准；configuration-guide 补缓存配置节 |
| 4 | CHANGELOG 宣称指标全改 `fulla_*` vs 代码实发 `oauth2_*`（authforge_* 前缀的已改） | **已在本 PR 修正 CHANGELOG 措辞** |
| 5 | JWKS 路径三个版本（/oauth2/jwks、/oauth2/.well-known/jwks.json、/.well-known/jwks.json） | DiscoveryController.cc:60 定谳 |
| 6 | PG 版本 15 vs 17（configuration-guide L38） | 17 |
| 7 | Google 路由 /google/login vs /api/google/login | Controller 定谳 |
| 8 | rbac-guide 单闸角色模型 vs api-reference 双闸（role+scope）vs "JWT roles 未来支持" | 现状=双闸+roles 已签发 |
| 9 | 测试数量 364+450 vs 501；e2e "7 文件/53 用例" vs 16/174 | 脚本与 spec 实测 |
| 10 | verification-checklist 的 oauth2-nginx(dev)/oauth2_migrations/WinDockerTest2024!/表名无前缀 | compose/V001/实配置定谳 |
| 11 | docs/README.md 索引摘要 "session unbounded leak ~730B" vs 调查报告 v2 已撤回（TTL 有界 750B） | 后者定谳；README 重写时消化 |
| 12 | 两个限流器（Hodor 全局 vs F-018 失败计数）从未说明并存关系与启用条件 | 重写时合并讲清 |

## 四、Docusaurus 内容源（核心决策不变，素材清单更新）

**站源 = docs/ 本体，零拷贝。** 站点七区与现状素材的映射（"现成"=SITE-READY，"重写后"=REWRITE/MERGE 完成）：

| 站区 | 内容源 | 状态 |
|---|---|---|
| intro | README 能力地图复用 + Quick Start（README Path A/B） | 现成 |
| architecture | architecture-overview + security-architecture（重写后）+ 模块地图 | 1 现成 1 重写 |
| domains | social-login（合并后）、oidc-guide（重写后）、rbac/access-control（重写后）、token-lifecycle（**新写**：吸收 data-persistence 一节+refresh family）、session-management（**新写**：吸收 drogon#278 结论）、multi-tenancy（**新写**） | 3 重写 + 3 新写 |
| sdk | sdk-integration-guide（吸收 plugin-integration）+ sdk-runtime-contract + 官方 Python/Go 说明（client-sdk 设计的"auth 手写"理由 200 字） | 基本现成 |
| operate | deployment（小修）、docker-deployment（吸收 docker-guide）、configuration-guide（重写后）、observability（重写后）、account-lockout、postgresql-major-upgrade、verification-checklist（重写后）、testing/e2e/ci-cd/versioning（contribute 区亦可） | 4 现成 4 重写 |
| benchmark | COMPARISON.md + **competitor-benchmark-design.md（新家 docs/benchmark/）** + benchmarks/README | 现成 |
| adr | **12 篇新转化 ADR** + ddd-domain-model（标注提案）+ 合规尽调报告（信任档案）+ rename-impact（脱敏后）+ professionalization-audit | 新建 |
| API | openapi.yaml 直渲染（swagger-ui 静态资源不上站，属服务器托管物） | 插件 |

双语（v4 修订）：**英文为主站语言**（fulla.dev 默认 locale = en，`/` 服务英文，
`/zh-CN` 服务中文，导航栏语言切换）。`docs/` 是**英文正本**（单一事实源）；
中文树镜像于 `website/i18n/zh-CN/docusaurus-plugin-content-docs/current/`（同构
目录、同 URL）。**翻译纪律**：改动 docs/ 英文文档的 PR 必须同步改中文对应文件
（同 PR 双写）；三份历史档案（合规尽调/改名影响/仓库审计）双语均为中文正文 +
英文语言说明头。GitHub 门面 README.md 为英文、README.zh-CN.md 为中文并链接
fulla.dev/zh-CN。

### 四·A、终稿信息架构（2026-08-26 定稿）

内容资产分**三层**，各层有明确边界与进入判据：

**第 1 层：`docs/`（英文正本，入库 + 上站）+ `website/i18n/zh-CN/`（中文译本）**
—— 站点内容源（Docusaurus 默认 locale 读 `../docs`，zh-CN locale 读 i18n 树，同构零漂移）。
只收"stranger 可据此完成一件事 / 理解一个决策 / 建立一份信任"的用户向内容：

```
docs/（英文）与 website/i18n/zh-CN/.../current/（中文）同构：
├── intro.md                     # 站点入口（快速路由表）
├── README.md                    # GitHub 侧索引（Docusaurus exclude，不上站）
├── documentation-governance.md  # 本文档（治理规则，贡献者区引用）
├── architecture/   # 评估+深潜：architecture-overview / security-architecture / data-persistence
├── domains/        # 领域指南：api-reference / oidc-guide / rbac-guide / social-login /
│                  #          token-lifecycle / session-management / multi-tenancy（Phase C 新增）
├── sdk/            # C++ SDK：sdk-integration-guide / sdk-runtime-contract
├── operate/        # 运维：deployment / docker-deployment / deployment-windows-docker-desktop /
│                  #        configuration-guide / observability / account-lockout /
│                  #        postgresql-major-upgrade / verification-checklist
├── contribute/     # 贡献：testing-guide / ci-cd-guide / versioning-and-release /
│                  #        admin-test-cases / user-frontend-test-cases / admin-e2e-testing-guide
├── benchmark/      # 竞品基准方法论（competitor-benchmark-design；结果表在仓库 benchmarks/）
└── adr/            # ADR-0001..0012 + 三份历史档案（中文正文 + 双语说明头）
```

**第 2 层：仓库内非 docs 资产（入库、不上站）** —— 站内以绝对链接引用，不复制：

| 资产 | 角色 |
|---|---|
| `README.md` / `README.zh-CN.md` | GitHub 门面（能力地图、Quick Start、徽章） |
| `benchmarks/competitors/results/COMPARISON.md` | 基准结果表（评估区链接） |
| `apps/server/openapi.yaml` | API 契约 SSoT（api-reference 导读指向它） |
| `.claude/rules/`、`TECH_SPECS.md`、`AGENTS.md` | 维护者契约（贡献者区可链接，不入站内容） |

**第 3 层：`docs-local/`（不入库，磁盘保留）** —— 维护者与 agent 的过程档案（history、
productization-evolution、branding、performance-optimization、performance 报告等）。
gitignore；判据：不满足第一层三判据（可执行/可决策/可信任）中任何一条的过程性文档。

**wiki 分工**：GitHub wiki 是自动生成的中文快照镜像（repowiki 转换），不做双向同步；
其 Home 指向 fulla.dev 为权威内容源（中文读者直达 /zh-CN）。站点与 wiki 内容重叠时以 `docs/` 为准。

**边界速判**：新文档先问"stranger 需要它吗？"——需要 → `docs/` 对应分区；只有维护者/
agent 需要 → `docs-local/`；是结果数据而非文档 → 仓库数据目录（如 `benchmarks/`）；
是对外承诺 → `docs/` + 版本化（CHANGELOG 引用）。

## 五、执行 Phase（v3 进度标注）

- **Phase A（内容修复与重组）——已完成**（8783c8e5 + 6049e3d3 + 7e5cd55a + d33d1ec3 + 45ca1f30）：
  A1 修矛盾登记簿 12 项 ✓（上站前三区复审补漏：verification-checklist/deployment-windows 残留口径二次清扫 ✓）；
  A2 六组 MERGE 落地 ✓；A3 ADR 转化 12 篇 ✓（status/source/双标题规范统一 ✓）；
  A4 出库 LOCAL 类 ✓（2 例外迁新家 ✓；**补执行缺口**：合规尽调报告/rename-impact（脱敏版）/
  professionalization-audit 三份档案此前误落 docs-local，现已入 docs/adr/ 并修复 CHANGELOG 两处断链 ✓）；
  A5 docs/README.md 重写 ✓。
  新增收尾：语言统一（全站简体中文，architecture-overview/configuration-guide 中文化 ✓）。
- **Phase B（站骨架）——已完成**（8a7e3b96）：website/ + 受众分区 sidebar + Pages 部署 + 死链守门。
  收尾项：GitHub Pages 源切换为 GitHub Actions（用户网页操作）+ 首页/主题专业化（本 PR）。
- **Phase C（内容补齐 + 双语化）——已完成**（docs/phase-c-i18n 分支）：
  三篇新写深潜（token-lifecycle / session-management / multi-tenancy）双语言落地 ✓；
  api-reference §6 已改为 OpenAPI 治理流程、正文统一简体（Phase A 收尾）✓；
  **英文主站 i18n**：docs/ 转为英文正本（~40 篇全量翻译），中文树迁至
  website/i18n/zh-CN/，导航栏语言切换，editUrlLocalized 双语言编辑链 ✓；
  deployment 会话节去重（尺寸表单一出处 = session-management）✓。
  持续义务：同 PR 双写（en 改动 ⇒ zh 同步）。

## 六、验收标准（不变，略增）

v1 四条全部保留，另加：⑤ 矛盾登记簿 12 项全部关闭；⑥ CHANGELOG 引用的文档链接零断链（合规报告、rename-impact §3）。
