# Documentation

The **user-facing** documentation tree, organized by audience (maintainer
process docs live outside the repo in `docs-local/`; criteria in
[documentation-governance.md](documentation-governance.md)). This tree is the
**English canonical** and the sole content source of
[fulla.dev](https://fulla.dev) (Docusaurus, zero copies); the Chinese
translation mirrors it at `website/i18n/zh-CN/` and is served at
[fulla.dev/zh-CN](https://fulla.dev/zh-CN).

## Evaluate

- [Architecture Overview](architecture/architecture-overview.md) — tech stack, module layout, authorization-code flow, storage strategy
- [Security Architecture](architecture/security-architecture.md) — threat model, token lifecycle, keys & hashes, security headers & rate limiting
- [Benchmark comparison](https://github.com/voidvec/fulla/blob/master/benchmarks/competitors/results/COMPARISON.md) ([methodology](benchmark/competitor-benchmark-design.md))

## Integrate

**C++ SDK (embedded)**: [Integration Guide](sdk/sdk-integration-guide.md) · [Runtime Contract](sdk/sdk-runtime-contract.md)

**HTTP API (any language)**: [API Reference](domains/api-reference.md) (authoritative contract:
[openapi.yaml](https://github.com/voidvec/fulla/blob/master/apps/server/openapi.yaml)) ·
[OIDC Integration](domains/oidc-guide.md) · [Social Login](domains/social-login.md) ·
[RBAC & Access Control](domains/rbac-guide.md)

## Deep dives

[Token Lifecycle](domains/token-lifecycle.md) · [Session Management](domains/session-management.md) · [Multi-Tenancy](domains/multi-tenancy.md) · [Data & Persistence](architecture/data-persistence.md)

## Operate

[Production Deployment](operate/deployment.md) · [Docker Deployment](operate/docker-deployment.md) ·
[Windows / Docker Desktop](operate/deployment-windows-docker-desktop.md) ·
[Configuration Guide](operate/configuration-guide.md) · [Observability](operate/observability.md) ·
[Account Lockout](operate/account-lockout.md) · [PostgreSQL Major Upgrades](operate/postgresql-major-upgrade.md) ·
[Deployment Verification Checklist](operate/verification-checklist.md)

## Contribute

[Testing Guide](contribute/testing-guide.md) · [CI/CD](contribute/ci-cd-guide.md) ·
[Versioning & Release](contribute/versioning-and-release.md) · [Documentation Governance](documentation-governance.md) ·
Frontend tests: [Admin cases](contribute/admin-test-cases.md) · [User cases](contribute/user-frontend-test-cases.md) ·
[E2E methodology](contribute/admin-e2e-testing-guide.md)

## Decision record (ADR)

[docs/adr/](adr/) — 13 current architecture decision records (SDK layering,
ErrorCatalog, opaque tokens, coroutine exclusion, …) plus three historical
archives: the [OAuth/OIDC compliance audit](adr/oauth-oidc-compliance-audit.md)
(2026-08-07 baseline, all 31 findings fixed), the
[rename impact analysis](adr/rename-impact-fulla.md), and the
[repo professionalization audit](adr/repo-professionalization-audit.md)
(the three archives are kept in Chinese with an English note).

> Earlier design archives (kiro specs in full, PRD designs) live in git
> history and maintainer-local storage; the ADRs distill the decisions that
> still hold.
