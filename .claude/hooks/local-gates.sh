#!/usr/bin/env bash
# local-gates.sh — 发版/推送前三门 + 迁移门 + 命名门一键预检（.claude/commands/preflight 的脚本形态）
# 用法: bash .claude/hooks/local-gates.sh   （从 repo 根运行；全部绿 exit 0）
# 已接线: .claude/settings.json 的 git commit PreToolUse 钩子（2026-09-10 取代旧的全量 ctest 预提交门
#         ——旧门指向不存在的 build/tests，实际会在 Claude Code 里阻断一切提交）
# 来源教训：v1.4.0/v1.0.0 两次 tag 秒挂均因这三门没在本地预跑（memory: release-version-sync-six-points）
set -u
cd "$(git rev-parse --show-toplevel)" || exit 1

fail=0
run_gate() {
  local name="$1"; shift
  printf '=== %s ... ' "$name"
  if "$@" >/tmp/.agent-gate.log 2>&1; then
    echo "PASS"
  else
    echo "FAIL (tail below)"
    tail -15 /tmp/.agent-gate.log
    fail=1
  fi
}

run_gate "spec-governance" python tools/openapi-governance/check_spec_governance.py
run_gate "sdk-drift"      python tools/clients/regen_clients.py --check
run_gate "api-diff"       python tools/api-diff/api_diff.py
run_gate "double-move"    python tools/arch-guard/double_move_guard.py
run_gate "migration"      python tools/migration-check/migration_check.py
run_gate "test-naming"    bash tools/test/scripts/naming_validator.sh

if [ "$fail" -ne 0 ]; then
  echo "RESULT: 有门未过——对照 .claude/skills/ci-gate-sync 的政策处理后重跑。"
  exit 1
fi
echo "RESULT: 全部门绿。"
