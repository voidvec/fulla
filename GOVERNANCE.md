# Governance

This document describes how decisions are made in the Fulla project and how
responsibility is distributed. It is intentionally minimal — it will grow with
the team, not ahead of it.

## Roles

### Maintainers

Maintainers have commit access and are responsible for review, release
engineering, and the long-term health of the codebase. The current list lives
in [MAINTAINERS.md](MAINTAINERS.md).

### Contributors

Anyone who submits issues, discussions, documentation, or pull requests.
Contributions are merged through the standard
[PR workflow](CONTRIBUTING.md) — there is no separate contributor tier.

## Decision process

| Decision class | Process |
|---|---|
| Bug fixes, docs, tests, refactors | PR + passing CI + one maintainer review |
| New features / behavior additions | Open an issue (or Discussion) first to agree on scope, then a PR |
| Breaking changes (API, config, protocol surface) | Same as features, plus: a curated entry in the `Breaking changes` section of [CHANGELOG.md](CHANGELOG.md), a version bump per the versioning policy, and OpenAPI/SDK surface sync (see `docs/`) |
| Security fixes | Private channel per [SECURITY.md](SECURITY.md); coordinated disclosure, then public PR/release |
| Governance itself (this file, MAINTAINERS.md, CODE_OF_CONDUCT.md) | Maintainer consensus |

Maintainers seek consensus. If consensus cannot be reached on a matter that
must be decided, the final call rests with the project founder (@voidvec),
who records the rationale in the relevant issue.

## Versioning and releases

Fulla follows [Semantic Versioning](https://semver.org/) for its published
artifacts (C++ SDK, Python/Go clients, container images). Releases are cut via
the automated pipeline (`.github/workflows/release.yml`) driven by version
tags; the release checklist and the six-point version sync list live in the
release-engineering skill (`.claude/skills/release-engineering/SKILL.md`),
with the version-source inventory in
[docs/contribute/versioning-and-release.md](docs/contribute/versioning-and-release.md) §1.

## Code of conduct

All participation is governed by the
[Code of Conduct](.github/CODE_OF_CONDUCT.md). Enforcement is handled by the
maintainers.
