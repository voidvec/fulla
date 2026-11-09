---
name: pr-workflow
description: fulla 仓库的 PR/issue/GitHub 工作流——rebase-merge 约定、冲突修复法（cherry-pick+patch-id 审计）、rebase 值丢失审计、分支清理、Fixes 纪律、gh PAT 权限边界、bug 分诊流程
---

# PR / GitHub 工作流

## 合并约定与冲突修复

- 用户对 master 一律 **Rebase and merge**（绝不 squash/merge-commit）。合并后本地 master 要
  `git reset` 到 `origin/master`（GitHub 重写了 commit hash）。
- PR 冲突的标准修法：把 PR 自己的提交 **cherry-pick 到最新 base** 上重植 → 验证 → force-push
  PR 分支。验证三件套：逐 commit `git patch-id --stable` 对比 PR 原始提交；最终
  `git diff <本地> origin/<pr分支>` 树差异必须为空；对 base 的净 diff 仍是 PR 原始范围。
- **GitHub 报的冲突可能是假阳性**（逐 commit 三方合并的标记）：本地 `git rebase origin/master`
  对最终树合并，两侧改动区域不重叠时会自动干净合上——先本地 rebase 试，别被冲突标记吓到就
  手工解。
- **rebase 会静默丢弃只存在于合并提交冲突解决里的值**（PR #64 实案丢了三处裁决值）。长链
  rebase 后必做审计：`git diff --name-only HEAD <备份分支>` 逐文件筛掉 base 动过的，其余 =
  纯丢失，从备份分支 checkout 恢复后补说明提交。
- CHANGELOG 冲突解法：把分支 `[Unreleased]` 内容放到新版本节**之前**（不能只删标记，否则
  Breaking 条目掉进已发布版本节）。

## 分支与提交纪律

- **修复合入的 commit 与 PR body 必须带 `Fixes #N`**（独立成行）——rebase/squash 任何合并方式
  都会触发自动关；漏写 = issue 漏关（#54 实案）。部分解决用 `Addresses #N`（不触发关闭）。
- 分支清理：rebase-merge 仓库里 `git branch -r --merged origin/master` **永远不显示**已合并
  分支（分支头是重写后的提交）——判定只能用 `gh pr list --state all --head <branch>` 的 PR
  状态。"Automatically delete head branches" **已失效**（2026-11-09 实测：#306/#319 合并后
  head 分支仍在）→ 合并后手动 `git push origin --delete <branch>`（先确认无 PR 引用）。
- **多会话/多 worktree 纪律**：动手前必查 `git branch --show-current` + `git reflog -5`（实案：
  另一会话切分支+reset 导致提交落错链）；主检出停留在用户要求的分支上，不要顺手切 master；
  要在 master 提交而工作区被占用 → `git worktree add $TEMP/xxx origin/master` 独立提交后 remove。

## push 与 gh 权限边界

- **push 必须 SSH**（HTTPS 403：credential-manager 旧 token + PAT 无 Contents write）；
  `--force-with-lease` 对 URL 推送须显式钉 `--force-with-lease=<ref>:<expected-oid>`。
- SSH 到 github.com 间歇超时 → fetch 临时切 HTTPS remote，push 前切回。
- gh PAT（细粒度）实测定谳（2026-11-09 复测）：**可用** = issue/PR 创建、PR 评论、
  `pr close --comment`、全部读、**PR merge**（`pr merge --rebase` 实落 #306/#319/#308）；
  **403** = issue 评论/关闭、repo 元数据编辑、release 删除/编辑、读分支保护规则
  （`gh api .../branches/master/protection`）、run rerun（后两项为旧测，撞到先重试再定谳）。
  撞 403 → 准备 paste-ready 文案交用户网页操作（用户偏好自己关 issue）。
- merge 的真实阻碍是策略不是权限：draft 要先 `gh pr ready`；"Require branch up to date"
  对 BEHIND 拒绝（rebase + force-push 解）；**REVIEW_REQUIRED 需外部批准——PR 作者无法自批**
  （#306 靠 wangrh0909，#308/#319 靠用户网页 Approve）；仓库未开 auto-merge（`--auto` 报
  GraphQL enablePullRequestAutoMerge 错）。
- CI 闪失重触发：**close→reopen** 触发 `reopened`（默认类型），不污染提交历史，优于推空
  提交。注意：push 发生在 PR 改 base **之前**会被 `pull_request: branches: [master]` 过滤，
  而 base 编辑的 `edited` 事件不在 workflow 默认触发类型里（#319 实案）。

## 跟踪类 issue 创建规范

用 gh/API 创建审计发现/评审遗留/技术债类 issue 时**英文**填写，套用
`.github/ISSUE_TEMPLATE/tracking.md` 骨架（Problem / Evidence / Suggested fix / Acceptance
criteria / Out of scope）——issue forms 只在网页流程生效，CLI 会绕过。面向用户的 bug/feature
走网页表单，不用 tracking 骨架。

## bug/issue 分诊（接手时的三分类）

1. **direct fix**：事实清楚、修法唯一、影响面小 → 直接修。
2. **plan-then-implement**：涉及架构/多文件/契约 → 先出方案（docs-local 计划文档 + 子代理
   评审）再实施。
3. **decision-required**：有真取舍（版本号、行为语义、对外契约）→ **方案摆好后停下问用户**，
   不要替用户拍板。

分诊的取证纪律：**issue 正文与代理报告的每条 load-bearing 断言都要对代码核实**（实案：issue
声称的"固定标识符冲突"实为崩溃残留；代理曾报错 userinfo CORS 缺失实为全局支持；评审建议
违反 RFC 6749 §6）。RFC 对照优先于直觉；"测试会失败"类预言要实际验证。

## 评审应对

- 外评意见逐条 RFC/代码核实后再修——建议可能违反规范（实案：删 refresh token 违反
  RFC 6749 §6；清 successMessage 抹掉刚设的消息）。
- 登记不修的项写成 tracking issue（走上面的骨架），PR 里注明编号。
- 需要用户决策的项集中列出（A/B 选项 + 推荐），一次问完。

## 并行协作（用户可能同时派 Qoder 等其它 AI）

- 分工原则：产品代码判断/CI triage/构建测试排程/PR 定稿/需问用户的决策归主代理；文件边界
  清晰、验收可脚本化、不写 build 目录的归 Qoder（报告落 `.zcode/plans/*/qoder-reports/`）。
- 硬边界：build 目录与 5555 端口主代理独占；Qoder 起服务前 `netstat` 确认 5555 空闲；提交前
  `git pull --rebase`；一任务一 commit。
