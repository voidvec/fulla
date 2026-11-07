# CLAUDE.md

This file is a thin pointer: Fulla keeps a single, tool-neutral set of
project instructions rather than per-assistant copies.

- **Read [AGENTS.md](AGENTS.md) first** — the cross-tool entry point. It
  indexes the authoritative rule files and module guides.
- **`.claude/rules/`** auto-loads path-scoped hard constraints:
  `db-operations`, `data-access`, `orm-models`, `dev-workflow`, `sync-points`,
  `frontend-contract`, `async-cpp-gotchas` (one-line summaries in the
  AGENTS.md rule index). When a rule there applies to files you are touching,
  follow it.
- Human-facing docs cover the rest: [CONTRIBUTING.md](CONTRIBUTING.md)
  (build/test, conventions, CI gates), [docs/](docs/) (architecture, testing,
  operations, SDK contracts), [README.md](README.md) (quick start).

`git push` is forbidden for agents (human review required); enforced as a deny
rule in `.claude/settings.json`.
