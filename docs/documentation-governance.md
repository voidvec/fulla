---
sidebar_label: Documentation Governance
---

# Documentation Governance v4 — Content Adjudication · Bilingual Docusaurus Site

> Status: v4 (2026-08-27). v1 directory-level triage → v2 per-file content
> adjudication from four parallel full-text reads (file:line evidence) →
> v3 final IA (three-layer model, Phases A/B done, pre-launch quality pass)
> → **v4: English-primary site with a zh-CN locale toggle** (docs/ is the
> English canonical; the Chinese tree lives under website/i18n/zh-CN/),
> Phase C deep-dives complete.
> Principle: **what ships in the repo is for strangers (they can get one
> thing done with it); what stays local is for maintainers and agents.**

## 1. In-repo criteria (unchanged)

1. **Actionable**: a stranger can get one thing done (deploy / integrate /
   troubleshoot / contribute);
2. **Decision-worthy**: records design decisions and rationale needed to
   understand the system (ADRs);
3. **Trust-worthy**: outward commitments (security posture, compliance
   audits, versioning policy, measurement reports and methodology).

## 2. Per-file adjudication (136 files + root docs) — historical record

### 2.1 docs/backend/ (23 md files + swagger-ui static assets)

| File | Verdict | Key evidence |
|---|---|---|
| architecture-overview.md | **SITE-READY** | All facts matched reality; add a line on the identity package and cache decorator |
| ci-cd-guide.md | **SITE-READY** | Verified against the 8 real workflows; add one-liners for clients-sdk/security |
| docker-deployment.md | **SITE-READY** | Matches compose reality; pool numbers updated per bench conclusions (64) |
| sdk-integration-guide.md | **SITE-READY** | Only the L8 `.kiro` source path was dead |
| sdk-runtime-contract.md | **SITE-READY** | Only the L7 `.kiro` path was dead |
| versioning-and-release.md | **SITE-READY** | Single governance home; L294 "840 commits" todo superseded by the version reset |
| api-reference.md | **REWRITE** | L226 end_session "no signature check" contradicted #78 (its own error table lists 4006); L254 Google route wrong; §6 taught the stale openapi.json workflow, conflicting with the governance gate |
| configuration-guide.md | **REWRITE** | L38 PG15→17; L69 cache layer described as "future" (shipped, config.json:152); L132 end_session stale semantics; L176 JWKS path wrong; env-var table listed only 5 of 30+ |
| data-persistence.md | **REWRITE** | §3 vs §6 self-contradicted on Redis-storage deprecation (L96 vs L205); schema stuck at V002 (now V026); `fulla:cache:` keyspace absent; absorb data-consistency + add delayed double-delete section |
| observability.md | **REWRITE** | Missing #80 cache-invalidation metrics; `oauth2_*` vs `fulla_*` naming never clarified; audit examples dated 2026-01 |
| oidc-guide.md | **REWRITE** | L21/34 JWKS routes wrong (actual `/.well-known/jwks.json` — copying the doc fails); missing end_session/backchannel integration duties, auth_time/acr/amr, official SDK channel |
| rbac-guide.md | **REWRITE** | L62 "roles in JWT — future" already implemented (TokenService.cc:334); scope layer (V023 dual-gate) entirely missing; manual SQL grants stale (admin API exists) |
| security-architecture.md | **REWRITE** | L36 secret-transport claim violated F-017 (Basic header default); threat table predated PR#85 fixes (#78 forged logout / #79 cache race / #54 soft-delete bypass) |
| testing-guide.md | **REWRITE** | L274 counts 364+450 (actual 501); L86-119 April snapshots; L195-263 four dangling "[DOC] (archived)" references |
| data-consistency.md | **MERGE → data-persistence** | Narrow but correct; missing #79 delayed double-delete |
| docker-guide.md | **MERGE → docker-deployment** | 60% overlap; L19 container names `oauth2-{service}` missed the rename; keep its uniques: naming table / debug containers / full_test_docker |
| google-guide.md + wechat-guide.md | **MERGE → social-login.md** | Isomorphic twins; google L45 no-button vs L53 click-button self-contradiction; lead with the wired-up GitHub flow |
| plugin-integration.md | **MERGE → sdk-integration-guide** | A quickstart subset of the latter's §3 |
| security-hardening.md | **MERGE → security-architecture** | Rate-limit numbers mismatched config.prod.json across the board (3/2/5 vs documented 5/5/10); drop April snapshots and dangling refs; note Hodor is prod-only |
| database-encoding-guide.md | **LOCAL** | Single-machine SQL_ASCII investigation; contains dangerous pg-catalog DELETE advice |
| documentation-standards.md | **LOCAL** | Repo-directory meta-rules, belongs with CONTRIBUTING; rewritten by this governance |
| ddd-domain-model.md | **ARCHIVE** | Self-described "unreviewed proposal"; honest mapping, an evolution draft — not a current-state doc |

### 2.2 docs/ops/ + admin/ + frontend/ + performance-optimization/ (16 files)

| File | Verdict | Key evidence |
|---|---|---|
| ops/account-lockout.md | **SITE-READY** | Solid; credential caliber needed unifying (conflict #1) |
| ops/postgresql-major-upgrade.md | **SITE-READY** | Fresh and accurate; fix L122 deploy name; **add to docs/README index (was missing)** |
| ops/deployment.md | **REWRITE (light)** | L571-583 initdb.d manual migration unexecutable in prod (migrations baked into the image); L783 Prometheus direct-connect vs loopback binding conflict; performance section already synced |
| ops/deployment-windows-docker-desktop.md | **REWRITE** | Test counts 55/51 stale (actual 59/52); "80% pass = success" bad caliber; 6 machine-local paths; admin123 credential conflict |
| ops/verification-checklist.md | **REWRITE** | Nonexistent nginx in the dev container table; `oauth2_migrations` table name wrong (actual schema_migrations); hardcoded password WinDockerTest2024!; table-count threshold >=7 stale; two different admin emails in one file |
| ops/security-checklist.md | **MERGE → backend/security-hardening** | Remediation-closeout memo; L76-84 two copy-paste-accident filter-branch commands |
| admin/e2e-testing-guide.md | **REWRITE (light)** | L903 dead link; appendix "7 files/53 cases" vs actual 16/174; §9 duplicated account-lockout wholesale |
| admin/test-cases.md | **SITE-READY** | No defects; healthy spec correspondence |
| frontend/test-cases.md | **SITE-READY** | No defects; covers the current feature surface |
| performance-optimization/ (all 7) | **LOCAL** | Prompts were AI-session artifacts; wave reports / instrumentation / non-code plans / memory investigations are internal evidence chains (baseline generations tangled — publishing would present three mutually contradictory QPS worldviews); user-facing conclusions were already extracted into ops/deployment §performance; upstream-drogon-session-issue.md is the sole record of the upstream constraint — link it once the issue is filed, then ARCHIVE |

### 2.3 docs/history/ (60 files): the ADR mine

**v1 correction**: superpowers/specs/ were not all session artifacts — 2 of them
are real design documents.

- **ADR conversion list (11 + 1 alternate, by priority)**:
  1. **Product + dual-SDK architecture and dependency iron rules** (sdk-refactor §2/§4.1/§5.2: Domain bans Drogon, oauth2/identity do not depend on each other, ports sink into common, arch-guard enforces)
  2. **Drogon self-registering-symbol linkage strategy** (sdk-refactor §5.5/§5.7: explicit registerController instead of whole-archive + plugin-zero-change option A)
  3. **ErrorCatalog single authority and dual-channel errors** (error-code AD-1..6 + auth-flow-gap "no-folding principle" and the G7 anti-enumeration exception)
  4. **Opaque access token + credential-hash storage + migration immutability** (production_hardening_spec §五; **note**: the decision table said Argon2id, the shipped implementation is PBKDF2-SHA256 310K — corrected during ADR conversion)
  5. **Email as the primary login identifier** (email-first §7, five decisions; V020 in tree)
  6. **Async callback lifetime patterns** (concurrency audit, four threads; CacheMap thread-safety conclusions)
  7. **MFA second-factor session binding** (mfa-fix: pending binding + same-code anti-enumeration; V022 in tree)
  8. **First-party SPA login credential-exposure control** (mfa_auth_code_pkce §6 revised + current authService.ts: AJAX+PKCE closure, tokens never in localStorage, hosted-login-page idea shelved)
  9. **ORM generated-model exemption + migration freeze** (repo-refactor §0/§1.2 — survived two refactors)
  10. **Rate-limiter choice: Hodor** (superpowers/specs: token-bucket three-tier limiting; config.prod uses it)
  11. **Integration-test platform tiering** (http-integration-plan: DB-backed tests Linux-only, in-process app, social surfaces unreachable)
  12. (Alternate) **PUBLIC/CONFIDENTIAL client authentication classes** (client-secret spec)
  Plus: distill "why C++17 bans coroutines" from async-refactor-assessment into a one-page ADR (every contributor asks).
- **ARCHIVE kept: 11 files** (history/README + bugfix/audit originals + 6 superseded designs);
- **LOCAL: 37 files** (all checkbox-style tasks/requirements/plans and process drafts);
- **DELETE: 1 file**: PRD/frontend_design.md (a strict subset of, and earlier snapshot than, frontend/oauth2_frontend_design.md — diff-verified).

### 2.4 productization-evolution/ + branding/ (25 files): two hard v1 corrections

- **LOCAL: 21 files** (including content-strategy "de-AI-flavor + advertorial process" — self-harming if public; progress-status enumerating unfixed security items #71/#73 — don't amplify; research containing unpublished pricing $499/$5000);
- **Exception 1 (SITE, benchmark zone): in-progress/competitor-benchmark-design.md stays in-repo** — three public entry points link it (README-badge COMPARISON.md, both READMEs, benchmarks/competitors/README); the content is publishable methodology (three-sames principle / official-config provenance / honest revision log); the environment disclosure (WSL2 8 vCPU/16GB) is reproducibility-required and already public; removing it breaks three public links. New home: `docs/benchmark/`;
- **Exception 2 (SITE, trust archive): done/oauth-oidc-compliance-audit.md stays in-repo** — an RFC compliance audit with all 31 findings fixed; publicly referenced from CHANGELOG.md:453; a trust asset for assessers (labeled "2026-08-07 baseline snapshot");
- **branding/rename-impact-fulla.md → ARCHIVE in-repo, sanitized first**: generalize machine paths and workspace details; compress the P0 "squat the assets" step; CHANGELOG:40 references its §3 — removal would break the link;
- **branding/repo-professionalization-audit.md → ARCHIVE in-repo**: AGENTS.md references it publicly as the in-repo standard;
- **branding/rename-candidates.md → LOCAL (highest sensitivity)**: exposes the unregistered status of fulla.dev + a 9-name availability list + self-critical assessments — intelligence for squatters until the assets are secured;
- Hygiene: `.mimosa/` session JSONs had leaked into docs/productization-evolution/ (untracked) → .gitignore `.mimosa/`.

## 3. Cross-document conflict register (Phase A must-fix list)

All conflicts found by the deep reads **had to be resolved before launch**,
otherwise the site would amplify them bilingually:

| # | Conflict | Settled by |
|---|---|---|
| 1 | **Three admin default-credential calibers** ('admin' vs admin123 vs admin/admin123+fulla-admin-console dual client) | Measured against apps/server/seed/dev_admin_user.sql; unified site-wide |
| 2 | end_session "no signature check" (api-reference L226, configuration-guide L132) vs the error table listing 4006 | Code enforces verification (#78); both passages rewritten |
| 3 | Redis cache layer "future" (configuration-guide L69) vs shipped (architecture-overview L12 pointed at the wrong section too) | config.json cache block is authoritative; configuration-guide gained a cache section |
| 4 | CHANGELOG claimed all metrics renamed `fulla_*` vs code emitting `oauth2_*` (the authforge_* ones were renamed) | **CHANGELOG wording fixed in that PR** |
| 5 | JWKS path in three versions | DiscoveryController.cc:60 settles it |
| 6 | PG version 15 vs 17 | 17 |
| 7 | Google route /google/login vs /api/google/login | The controller settles it |
| 8 | rbac-guide single-gate roles vs api-reference dual-gate (role+scope) vs "JWT roles — future" | Current state = dual gate + roles issued |
| 9 | Test counts 364+450 vs 501; e2e "7 files/53 cases" vs 16/174 | Scripts and specs measured |
| 10 | verification-checklist's oauth2-nginx(dev)/oauth2_migrations/WinDockerTest2024!/unprefixed table names | compose/V001/actual config settle it |
| 11 | docs/README.md summary "session unbounded leak ~730B" vs investigation v2's retraction (TTL-bounded 750B) | The latter; absorbed in the README rewrite |
| 12 | Two rate limiters (Hodor global vs F-018 failure counting) with their coexistence never explained | Explained together in the rewrite |

## 4. Docusaurus content source (decision unchanged, inventory updated)

**Site source = the repo's docs trees, zero copies.** Zone mapping (all rows
now executed):

| Zone | Source | Status |
|---|---|---|
| intro | README capability map + Quick Start (README Path A/B) | done |
| architecture | architecture-overview + security-architecture + data-persistence | done |
| domains | api-reference, oidc-guide, rbac-guide, social-login, **token-lifecycle, session-management, multi-tenancy (Phase C)** | done |
| sdk | sdk-integration-guide + sdk-runtime-contract + official Python/Go clients | done |
| operate | deployment, docker-deployment, deployment-windows-docker-desktop, configuration-guide, observability, account-lockout, postgresql-major-upgrade, verification-checklist | done |
| benchmark | COMPARISON.md (repo) + competitor-benchmark-design.md (docs/benchmark/) | done |
| adr | ADR-0001..0012 + three trust archives | done |
| API | openapi.yaml rendered at runtime by the server (swagger-ui static assets are server-hosted, not site content) | n/a |

Bilingual policy (v4): **English is the primary site language** (fulla.dev
default locale = en — `/` serves English, `/zh-CN` serves Chinese, navbar
language switcher). `docs/` is the **English canonical** (single source of
truth); the Chinese tree mirrors it at
`website/i18n/zh-CN/docusaurus-plugin-content-docs/current/` (identical
layout, identical URLs). **Translation discipline**: a PR that changes an
English doc under docs/ must update the Chinese counterpart in the same PR.
The three historical archives keep their Chinese body in both locales with an
English language-note header. The GitHub-facing README.md is English;
README.zh-CN.md is Chinese and links to fulla.dev/zh-CN.

### 4A. Final information architecture (settled 2026-08-26, bilingualized 2026-08-27)

Content assets live in **three layers**, each with clear boundaries and
entry criteria:

**Layer 1: `docs/` (English canonical, in-repo, on-site) +
`website/i18n/zh-CN/` (Chinese translation)** — the site content source
(Docusaurus default locale reads `../docs`, the zh-CN locale reads the i18n
tree; identical structure, no drift). Only content where "a stranger can get
one thing done / understand one decision / establish one trust":

```
docs/ (English) and website/i18n/zh-CN/.../current/ (Chinese), mirrored:
├── intro.md                     # site entry (routing table)
├── README.md                    # GitHub-side index (excluded from the site)
├── documentation-governance.md  # this document
├── architecture/   # evaluate + deep-dives: architecture-overview / security-architecture / data-persistence
├── domains/        # domain guides: api-reference / oidc-guide / rbac-guide / social-login /
│                  #              token-lifecycle / session-management / multi-tenancy (Phase C)
├── sdk/            # C++ SDK: sdk-integration-guide / sdk-runtime-contract
├── operate/        # operations: deployment / docker-deployment / deployment-windows-docker-desktop /
│                  #             configuration-guide / observability / account-lockout /
│                  #             postgresql-major-upgrade / verification-checklist
├── contribute/     # contributing: testing-guide / ci-cd-guide / versioning-and-release /
│                  #               admin-test-cases / user-frontend-test-cases / admin-e2e-testing-guide
├── benchmark/      # competitor-benchmark methodology (results live in the repo's benchmarks/)
└── adr/            # ADR-0001..0012 + three historical archives (Chinese body, bilingual note)
```

**Layer 2: in-repo non-docs assets (tracked, not site content)** — referenced
by absolute link, never copied:

| Asset | Role |
|---|---|
| `README.md` / `README.zh-CN.md` | GitHub front door (capability map, Quick Start, badges) |
| `benchmarks/competitors/results/COMPARISON.md` | Benchmark results (evaluate-zone link) |
| `apps/server/openapi.yaml` | API contract SSoT (api-reference points at it) |
| `.claude/rules/`, `TECH_SPECS.md`, `AGENTS.md` | Maintainer contracts (linkable from contribute, not site content) |

**Layer 3: `docs-local/` (untracked, on disk)** — maintainer/agent process
archives (history, productization-evolution, branding,
performance-optimization, perf reports). gitignored; criterion: process
documents that satisfy none of the three layer-1 criteria.

**Wiki division of labor**: the GitHub wiki is an auto-generated Chinese
snapshot mirror (repowiki conversion), not bidirectionally synced; its Home
points to fulla.dev as the authoritative source (Chinese readers go straight
to /zh-CN). When site and wiki overlap, `docs/` wins.

**Quick boundary test for a new document**: ask "does a stranger need it?" —
yes → the matching docs/ zone (English) + the zh tree; only maintainers/
agents → docs-local/; it's results data, not documentation → a repo data
directory (e.g. `benchmarks/`); it's an outward commitment → docs/ +
versioned (referenced from CHANGELOG).

## 5. Execution phases (v4 progress)

- **Phase A (content repair & reorganization) — done** (8783c8e5 + 6049e3d3
  + 7e5cd55a + d33d1ec3 + 45ca1f30): all 12 register conflicts closed
  (including the second sweep of verification-checklist / deployment-windows
  residuals); six MERGE groups landed; 12 ADRs converted (status/source/
  duplicate-title normalized); LOCAL classes removed from the repo (both
  exceptions rehomed; **execution gap closed**: the compliance audit /
  sanitized rename-impact / professionalization-audit archives, initially
  left in docs-local, moved into docs/adr/ with the two CHANGELOG links
  fixed); docs/README rewritten; language unified to simplified Chinese
  (superseded by v4's bilingual model).
- **Phase B (site skeleton) — done** (8a7e3b96): website/ + audience-zoned
  sidebar + Pages deployment + broken-link gate. Close-out: Pages source
  switched to GitHub Actions (user action) + professional landing/theme.
- **Phase C (content completion + bilingualization) — done**
  (docs/phase-c-i18n): three new deep-dives (token-lifecycle /
  session-management / multi-tenancy) in both languages; api-reference §6
  rewritten around the OpenAPI governance gate; **English-primary i18n**:
  docs/ converted to the English canonical (~40 files translated), the
  Chinese tree moved to website/i18n/zh-CN/, navbar locale switcher,
  editUrlLocalized edit links per locale; the deployment doc's session
  section de-duplicated (single home for the sizing table =
  session-management). Standing obligation: same-PR dual writes (en change
  ⇒ zh sync).

## 6. Acceptance criteria (unchanged, extended)

All four v1 criteria hold, plus: ⑤ the 12-item conflict register fully
closed; ⑥ zero broken links from CHANGELOG-referenced docs (compliance
report, rename-impact §3); ⑦ (v4) every doc exists in both locales and both
builds pass the broken-link gate.
