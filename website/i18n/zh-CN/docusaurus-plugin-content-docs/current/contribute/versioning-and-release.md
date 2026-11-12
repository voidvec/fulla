# Versioning & Release Policy

Fulla 的版本号方案、bump 判定规则、发布节奏、预发布与补丁通道，以及
版本发布的标准操作流程（SOP）。

本文档是版本治理（governance）的单一出处。**版本工程的"怎么做"（CI 流水
线、签名、SBOM）由 [`.github/workflows/release.yml`](https://github.com/voidvec/fulla/blob/master/.github/workflows/release.yml)
实现；本文回答的是"何时发版、bump 什么、为什么"。** 二者冲突时，以本文为
准并修流水线。

> 相关文档：
> - [SDK Runtime Contract](../sdk/sdk-runtime-contract.md) §2 声明了 ABI / 源码级
>   SemVer 承诺与弃用流程，本文是其版本治理侧的展开。
> - [CI/CD Guide](ci-cd-guide.md) 描述 release 流水线在整体 CI 中的位置。

---

## 1. 版本号方案

### 1.1 SemVer 2.0.0

Fulla 遵循 [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html)：

```
MAJOR.MINOR.PATCH[-prerelease]
   1  .  0 .  0  -rc.1
```

| 段 | bump 依据（简述，详见 §2 决策表） | 兼容性承诺 |
|---|---|---|
| **MAJOR** | 破坏性变更（breaking change） | 无 —— 用户需改代码 |
| **MINOR** | 新增功能、向后兼容 | 源码兼容 |
| **PATCH** | 向后兼容的缺陷修复 | 源码兼容 |
| **prerelease** | `-alpha.N` / `-beta.N` / `-rc.N` | 无承诺 |

> v1.x 的"源码兼容"边界由 [SDK Runtime Contract](../sdk/sdk-runtime-contract.md) §2
> 明确：仅覆盖 `libs/*/include/fulla/**` 公共头的**源码级 API**，不承诺
> 二进制 ABI。

### 1.2 版本号单一来源（SSoT）

| 组件 | 版本来源 | 同步校验 |
|---|---|---|
| C++ 库 + server | [`cmake/Version.cmake`](https://github.com/voidvec/fulla/blob/master/cmake/Version.cmake) 的 `MAJOR/MINOR/PATCH` | ✅ `tools/api-diff/api_diff.py` 交叉校验 `Version.cmake` / `CMakeLists.txt project(VERSION)` / `conanfile.py version` |
| 前端 SPA | `frontends/admin/package.json` + `frontends/user/package.json` | 六点同步清单（见下） |
| OpenAPI 规格 | `apps/server/openapi.yaml` 的 `info.version` | ✅ OpenAPI 治理门校验 == `Version.cmake` |
| Python 客户端 | `clients/python/pyproject.toml` 的 `version` | ✅ `regen_clients` 漂移门校验 == `Version.cmake` |
| TypeScript 客户端 | `clients/ts/package.json` 的 `version` | v1.6.0 起并入 pyproject == cmake 版本联动 |
| Docker 镜像 | 由 `release.yml` 从 `Version.cmake` 读取 | GHCR tag = `<version>` |
| 中文文档镜像 | `website/i18n/zh-CN/**` 中内嵌具体版本号的页面（如 `sdk-integration-guide` 携带 tarball 名 / `CMAKE_PREFIX_PATH` / GHCR tag 示例） | 同 PR 双写义务；**明确列入版本载体清单**——v1.5.0 与 v1.6.0 两次升版都漏了这些页面 |
| DB schema | `apps/server/migrations/V0NN_*.sql` 编号 | **不耦合产品版本**（见 §6） |

**任何版本发布的第一步都是改 `cmake/Version.cmake`**；版本号漂移会被
`api-diff` 在 `release.yml` 的 `version-check` job 拦截。

> 权威的**六点版本同步清单**（tag 门会挂的最小集）与约 15 处额外版本载体，
> 见维护者 **release-engineering skill**：
> [`.claude/skills/release-engineering/SKILL.md`](https://github.com/voidvec/fulla/blob/master/.claude/skills/release-engineering/SKILL.md)
> ——本表只是摘要；发版时以该 skill 为走查清单。

---

## 2. 版本号 bump 决策表

一个改动到底触发 MAJOR / MINOR / PATCH bump？按下表判定。**当多行命中时，
取最高级别（MAJOR > MINOR > PATCH）。**

| 改动类型 | → MAJOR | → MINOR | → PATCH |
|---|:---:|:---:|:---:|
| SDK 公共头**删除 / 重命名 / 签名变更 / 默认实参变更**（api-diff 判为 BREAKING） | ✅ | | |
| 公共 API **行为语义变更**（返回值含义、错误码、副作用、协议字段语义） | ✅ | | |
| 最低 C++ 标准 / 编译器版本提升 | ✅ | | |
| Drogon / Postgres / Redis **大版本**依赖升级 | ✅ | | |
| 配置项**删除**或**改默认值且旧行为无法兼容** | ✅ | | |
| DB schema 的**破坏性 migration**（删列 / 改类型无回填 / 重命名） | ✅ | | |
| 新增 SDK API / 新 OAuth2 端点 / 新 OIDC claim | | ✅ | |
| 已有 API 的**新增可选参数 / 字段**（带默认值） | | ✅ | |
| 新增可选配置项（旧配置仍可工作） | | ✅ | |
| 新增可选依赖 | | ✅ | |
| 性能优化（不改公共 API） | | ✅ | |
| `feat:` conventional commit（无 `!`） | | ✅ | |
| `fix:` conventional commit —— API 行为回归到"正确" | | | ✅ |
| 安全漏洞修复（CVE 类，不改 API） | | | ✅ |
| 文档 / 测试 / CI 修复（若决定发版） | | | ✅ |
| 纯 `docs: / test: / chore: / build: / ci:` 提交 | | | 不发版 |

### Conventional Commits → bump 自动映射

提交前缀与 bump 的默认映射（`!` 后缀或 `BREAKING CHANGE:` footer 强制升
MAJOR）：

```
feat:     → MINOR      feat!:    → MAJOR
fix:      → PATCH      fix!:     → MAJOR
perf:     → PATCH      perf!:    → MAJOR
refactor: → 不发版（除非 !）
docs/test/chore/build/ci: → 不发版
```

scope 不改变默认映射，但 maintainer 可按 scope 上调（见 §3）。`cliff.toml`
的 commit parser 已与上表对齐。

---

## 3. 安全 hardening 的"灰色地带"——显式取舍声明

OAuth/OIDC 合规审计产生的一类改动特殊：它们**收紧了原本宽松的行为**（例：
强制 redirect_uri https、强制 PKCE、CONFIDENTIAL 客户端 refresh grant 强制
client_secret）。这类改动：

- **严格 SemVer 视角**：是 breaking（依赖旧宽松行为的下游会断）。
- **行业惯例**：多在 MINOR bump 内推进，Release Notes 显著标注。

**Fulla 的取舍：**

> 安全 hardening 在 **MINOR** 内推进，**不强制 MAJOR**。理由：本项目此前的
> "宽松行为"本身就是 spec 违规（bug），修复是回归正确，不是产品语义的有意
> 改变。但每次此类改动**必须**在 Release Notes 的 **⚠️ Breaking (security
> hardening)** 小节显式列出，并给出迁移指引。

这是**显式权衡**，不是含糊处理。若某次 hardening 的影响面经评估确实广泛
（例：移除整个 grant type），仍应走 MAJOR + pre-release 通道（见 §5）。

---

## 4. 发布节奏（cadence）

采用**混合模式**：周期性 MINOR + 按需 PATCH + 紧急安全 hotfix。

| 事件 | 触发条件 |
|---|---|
| **计划性 MINOR**（新功能） | 每 **4–6 周** 一次；或累计 ≥ 3 个 `feat:` commit 时 |
| **PATCH**（bug fix） | 累计 ≥ 5 个 `fix:` commit；或有用户报告的 bug 已修复 |
| **紧急安全 PATCH** | P0 / CVE 漏洞修复后**立即**（不等 cadence） |
| **MAJOR** | 有 breaking change 累积时；必须走 pre-release 通道（§5） |

节奏是**指引不是教条**：无值得发版的改动时跳过一个周期完全正常；反之 P0
安全修复永远立即发版。

---

## 5. Pre-release 通道

正式 MAJOR 发布前走阶梯式预发布：

```
v2.0.0-alpha.1 → alpha.2 → … → v2.0.0-beta.1 → … → v2.0.0-rc.1 → … → v2.0.0
```

| 阶段 | 语义 | 接受的改动 |
|---|---|---|
| `alpha.N` | 功能可能不全，CI 不保证通过 | 任何（含新功能、行为调整） |
| `beta.N` | 功能冻结，征集反馈 | bug fix + 反馈驱动的非破坏调整 |
| `rc.N` | 发布候选 | **仅** P0/P1 bug fix |
| （去后缀） | 正式版 | 不接受新改动 |

**镜像 tag 规则**：
- 正式版 → 打 `<version>` **和** `latest`
- pre-release → **只打** `<version>`（如 `v2.0.0-rc.1`），**不打** `latest`

> **当前流水线状态**：`release.yml` 的 tag 触发模式 `v[0-9]+.[0-9]+.[0-9]+`
> **不接受后缀**，故 pre-release tag 当前不会触发发版。启用 pre-release
> 通道需扩展该正则以匹配 `v[0-9]+.[0-9]+.[0-9]+(-[a-z]+\.[0-9]+)?`，并在
> `github-release` job 按 tag 是否含 `-` 决定是否标记 GitHub Release 为
> "Pre-release" 且跳过 `latest` manifest 合并。这是本 policy 的**待实施
> 改造项**（见 §11）。

---

## 6. DB Schema 版本与产品版本解耦

Fulla 用编号式 migration（`V001_*` … `V0NN_*`），编号独立递增。

- **新增 migration（加表 / 加列 / 加索引）** = 向后兼容 → 触发 **MINOR** 评估。
- **破坏性 migration（删列 / 改类型无回填 / 重命名）** → 触发 **MAJOR** 评估。
- Schema 版本表只记录 migration 应用历史，**不**映射到 `MAJOR.MINOR.PATCH`。

---

## 7. Release Branch 与 Patch Release

当 v1.2.0 发布后，主线开发 v1.3.0。若 v1.2.0 发现 P0 漏洞：

```
master:  ──●──●──●──●──●──●──→  (开发 v1.3.0)
               \
release/1.2:    └──●(cherry-pick fix)──● tag v1.2.1
```

**约定**：
- 分支命名：`release/<MAJOR>.<MINOR>`（如 `release/1.2`）。
- 该分支**只接受 cherry-pick 的 bug fix**，不接受新功能。
- 每个 patch release 打一个 `v<MAJOR>.<MINOR>.<PATCH>` tag，触发 `release.yml`。
- **维护窗口**：仅维护**最新一个** release branch 的 patch。上一个 branch
  在新 minor 发布后 EOL（不提供 LTS —— 见 §8）。

---

## 8. LTS（长期支持）

**当前阶段不提供 LTS。** 仅维护最新 minor 的 patch release。当下游用户规模
增长、升级成本显现时再评估是否引入 LTS（如 Node.js / Kubernetes 模式）。

---

## 9. `latest` tag 语义与生产部署

- `:latest` 指向**最新正式版**（不含 pre-release）。
- [`deploy/docker/docker-compose.prod.yml`](https://github.com/voidvec/fulla/blob/master/deploy/docker/docker-compose.prod.yml)
  使用 `${FULLA_VERSION:-latest}`：部署便利的默认值。
- ⚠️ **生产部署应显式 pin 到具体版本号**（`FULLA_VERSION=1.2.0`），
  不要依赖 `latest` —— 它会在新版本发布时不可控地滚动。

---

## 10. 弃用流程（Deprecation）

与 [SDK Runtime Contract](../sdk/sdk-runtime-contract.md) §2 一致：

1. 在当次 MINOR 发布时，用 `[[deprecated("Use X instead; removed in vN.0")]]`
   标注被弃用的 API。
2. Release Notes 的 **Deprecated** 段记录该弃用 + 迁移指引。
3. **至少保留一个 MINOR 周期**（建议两个）。
4. 在下一个 MAJOR 删除。

非 SDK 的弃用（配置项、端点参数）遵循同样的"标注 → 过渡 → 删除"流程，
标注方式用 Release Notes + 配置加载时的 LOG_WARNING。

---

## 11. 版本发布标准操作流程（SOP）

### 11.0 发版前三件套门槛（阻断项，每次发版）

改进路线图 §7 DoD 的三件套：**三件齐备才能打 tag**。v1.5.0 发版漏跑了第 1 件、
对比只能事后补进 v1.6.0 战役（`deliverables/five-scenario-benchmark-2026-10-27.md`）——
本节把门槛写死，避免再次被无声跳过。

1. **五场景性能对比 ≥ 90%**：发版候选与上一 tag 的同机背靠背对比
   （benchmark-methodology 纪律：宿主零负载、独立栈、wrk 楼梯、复跑 + 同窗口对照臂）。
   报告落 `deliverables/`、原始数据落 `benchmarks/results/<date>-<sha>-*`，
   版本 bump PR 引用两者（v1.6.0 起的既定惯例）。判定采信**同窗口对照**，不是首臂数据。
2. **CHANGELOG 完备**：`[vX.Y.Z]` 节成稿、Breaking 置顶（release-notes splice 缺节即挂）。
3. **版本六点同步**：release-engineering 清单（cmake SSoT → 全部载体；双 SDK 清单；
   前端；helm/compose；docs），以 `regen_clients.py --version-only` + spec 治理门验证。

### 11.1 正式版 MINOR / PATCH（从 master）

```sh
# 0. 五场景门槛（见 11.0）：报告 + 原始数据先入库，bump PR 引用两者

# 1. 确认 master 绿（CI 全过）
git checkout master && git pull

# 2. 更新版本号 SSoT
#    编辑 cmake/Version.cmake 的 MINOR 或 PATCH

# 3. 校验 API 表面（关键步骤）
python3 tools/api-diff/api_diff.py
#   - additive drift（新增头/新增声明）→ MINOR 允许，ratify:
#       python3 tools/api-diff/api_diff.py --update-baseline
#   - breaking drift（删/改声明）→ 必须先确认 MAJOR 已 bump；
#     若属"私有成员/include 重排"等不影响消费表面的改动，需 review 后:
#       python3 tools/api-diff/api_diff.py --force --update-baseline

# 4. 生成 CHANGELOG 草稿，再手工策展
git cliff --unreleased --tag vX.Y.Z --prepend CHANGELOG.md
#   手工编辑要点：
#     - 归类到 Added / Fixed / Changed / Security / Deprecated / ⚠️ Breaking
#     - 安全 hardening 进 ⚠️ Breaking (security hardening) 小节 + 迁移指引
#     - 删除无信息量的条目

# 5. 提交版本号 + baseline + CHANGELOG
git add cmake/Version.cmake tools/api-diff/*.baseline CHANGELOG.md
git commit -m "chore(release): vX.Y.Z"

# 6. 打 tag 并推送 —— 触发 release.yml
git tag vX.Y.Z
git push origin master --tags
```

`release.yml` 自动完成：version-check → SDK tarball → 多架构镜像 → cosign
签名 → SBOM → GitHub Release（含 git-cliff 生成的 notes + 验证指引）。

### 11.2 紧急安全 PATCH（从 release branch）

```sh
# 1. 基于 release/<MAJOR>.<MINOR> 分支 cherry-pick fix commit
git checkout release/1.2
git cherry-pick <fix-commit-sha>

# 2. 在该分支上 bump PATCH（步骤同 11.1 的 2–6，但操作对象是 release branch）
```

### 11.3 MAJOR（走 pre-release 通道）

```sh
# 1. 在 master（或专用候选分支）上累积 breaking 改动
# 2. bump MAJOR，依次打 prerelease tag：
git tag v2.0.0-alpha.1 && git push --tags   # → alpha 阶段
# ... 反馈迭代 ...
git tag v2.0.0-beta.1  && git push --tags   # → beta 阶段
git tag v2.0.0-rc.1    && git push --tags   # → rc 阶段（仅修 P0/P1）
# 3. rc 通过后去掉后缀即正式版：
git tag v2.0.0         && git push --tags
# 4. 正式版发布后创建 release/2.0 分支
git checkout -b release/2.0 v2.0.0 && git push origin release/2.0
```

> ⚠️ §5 已述：pre-release tag 当前**不触发** `release.yml`。启用此通道前
> 需先完成流水线改造（见 §12 待办）。

---

## 12. 待办（本 policy 与现状的差距）

| # | 项 | 解决的问题 |
|---|---|---|
| **T3** | 扩展 `release.yml` tag 触发模式 + `latest` 跳过逻辑，启用 pre-release 通道 | 当前 prerelease tag 不触发发版 |
| **T4** | 在 prod 部署文档（`docker-deployment.md`）加 `latest` 警告交叉引用 | 默认 `latest` 对生产有滚动风险 |
| **T5** | 定义 release branch 命名约定并在 README 加指针（首次实际需要时再立分支） | patch release 流程未实例化 |

T1（本文件）与 T2（v1.0.0 后的首次发版）已完成（首个后续版本为 v1.0.1）；T3–T5 可在对应场景首次出现时落地。
