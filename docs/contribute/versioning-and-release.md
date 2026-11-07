# Versioning & Release Policy

fulla's version numbering scheme, bump decision rules, release cadence, pre-release
and patch channels, and the standard operating procedure (SOP) for shipping a
release.

This document is the single source of truth for versioning governance. **The "how"
of release engineering (CI pipeline, signing, SBOM) is implemented by
[`.github/workflows/release.yml`](https://github.com/voidvec/fulla/blob/master/.github/workflows/release.yml); this
document answers "when to release, what to bump, and why".** When the two
conflict, this document wins — fix the pipeline.

> Related documents:
> - [SDK Runtime Contract](../sdk/sdk-runtime-contract.md) §2 declares the ABI /
>   source-level SemVer commitments and the deprecation process; this document
>   expands on their versioning-governance side.
> - [CI/CD Guide](ci-cd-guide.md) describes where the release pipeline
>   sits in the overall CI.

---

## 1. Version Numbering Scheme

### 1.1 SemVer 2.0.0

fulla follows [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html):

```
MAJOR.MINOR.PATCH[-prerelease]
   1  .  0 .  0  -rc.1
```

| Segment | Bump trigger (summary; see the decision table in §2) | Compatibility commitment |
|---|---|---|
| **MAJOR** | Breaking change | None — users must change their code |
| **MINOR** | New functionality, backwards compatible | Source compatible |
| **PATCH** | Backwards-compatible defect fixes | Source compatible |
| **prerelease** | `-alpha.N` / `-beta.N` / `-rc.N` | No commitment |

> The boundary of "source compatible" for v1.x is defined by
> [SDK Runtime Contract](../sdk/sdk-runtime-contract.md) §2: it covers only the
> **source-level API** of the public headers under `libs/*/include/fulla/**` and
> makes no binary-ABI commitment.

### 1.2 Single source of truth for the version number (SSoT)

| Component | Version source | Sync validation |
|---|---|---|
| C++ libraries + server | `MAJOR/MINOR/PATCH` in [`cmake/Version.cmake`](https://github.com/voidvec/fulla/blob/master/cmake/Version.cmake) | ✅ `tools/api-diff/api_diff.py` cross-checks `Version.cmake` / `CMakeLists.txt project(VERSION)` / `conanfile.py version` |
| Frontend SPAs | `frontends/admin/package.json` + `frontends/user/package.json` | Six-point sync list (see below) |
| OpenAPI spec | `apps/server/openapi.yaml` `info.version` | ✅ OpenAPI governance gate checks it equals `Version.cmake` |
| Python client | `clients/python/pyproject.toml` `version` | ✅ `regen_clients` drift gate checks it equals `Version.cmake` |
| TypeScript client | `clients/ts/package.json` `version` | Joins the `pyproject` == cmake linkage since v1.6.0 |
| Docker images | Read from `Version.cmake` by `release.yml` | GHCR tag = `<version>` |
| Chinese doc mirrors | `website/i18n/zh-CN/**` pages that embed concrete versions (e.g. `sdk-integration-guide` carries the tarball name / `CMAKE_PREFIX_PATH` / GHCR tag examples) | Same-PR dual-write duty; **explicitly on the carrier list** — the v1.5.0 and v1.6.0 bumps both missed these pages |
| DB schema | The numbering of `apps/server/migrations/V0NN_*.sql` | **Not coupled to the product version** (see §6) |

**The first step of any release is editing `cmake/Version.cmake`**; version drift
is intercepted by `api-diff` in the `version-check` job of `release.yml`.

> The authoritative **six-point version sync list** (the minimum set that fails
> the tag gate) plus the ~15 additional version carriers lives in the maintainer
> **release-engineering skill** at
> [`.claude/skills/release-engineering/SKILL.md`](https://github.com/voidvec/fulla/blob/master/.claude/skills/release-engineering/SKILL.md)
> — this table summarizes it; the skill is the checklist to walk when cutting a
> release.

---

## 2. Version Bump Decision Table

Which bump does a change trigger — MAJOR / MINOR / PATCH? Decide with the table
below. **When multiple rows match, take the highest level (MAJOR > MINOR >
PATCH).**

| Change type | → MAJOR | → MINOR | → PATCH |
|---|:---:|:---:|:---:|
| SDK public header **removed / renamed / signature changed / default argument changed** (judged BREAKING by api-diff) | ✅ | | |
| **Behavioral semantics change** of a public API (meaning of return values, error codes, side effects, protocol field semantics) | ✅ | | |
| Raise of the minimum C++ standard / compiler version | ✅ | | |
| **Major-version** dependency upgrade of Drogon / Postgres / Redis | ✅ | | |
| Config option **removed**, or **default value changed with no compatible old behavior** | ✅ | | |
| **Breaking migration** of the DB schema (column drop / type change without backfill / rename) | ✅ | | |
| New SDK API / new OAuth2 endpoint / new OIDC claim | | ✅ | |
| **New optional parameter / field** on an existing API (with default value) | | ✅ | |
| New optional config option (old configs keep working) | | ✅ | |
| New optional dependency | | ✅ | |
| Performance optimization (no public API change) | | ✅ | |
| `feat:` conventional commit (no `!`) | | ✅ | |
| `fix:` conventional commit — API behavior regressing back to "correct" | | | ✅ |
| Security vulnerability fix (CVE-type, no API change) | | | ✅ |
| Docs / tests / CI fixes (if a release is decided) | | | ✅ |
| Purely `docs: / test: / chore: / build: / ci:` commits | | | No release |

### Conventional Commits → bump automatic mapping

The default mapping from commit prefix to bump (an `!` suffix or a
`BREAKING CHANGE:` footer forces MAJOR):

```
feat:     → MINOR      feat!:    → MAJOR
fix:      → PATCH      fix!:     → MAJOR
perf:     → PATCH      perf!:    → MAJOR
refactor: → no release (unless !)
docs/test/chore/build/ci: → no release
```

A scope does not change the default mapping, but a maintainer may raise the level
based on the scope (see §3). The commit parser in `cliff.toml` is already aligned
with the table above.

---

## 3. The security-hardening "gray zone" — an explicit trade-off statement

One category of changes arising from OAuth/OIDC compliance audits is special:
they **tighten previously lenient behavior** (e.g. enforcing https
redirect_uri, enforcing PKCE, requiring client_secret for the refresh grant of
CONFIDENTIAL clients). Such changes:

- **From a strict SemVer perspective**: breaking (downstreams relying on the old
  lenient behavior break).
- **Industry practice**: mostly shipped within a MINOR bump, prominently flagged
  in the Release Notes.

**fulla's trade-off:**

> Security hardening ships within a **MINOR** and does **not force a MAJOR**.
> Rationale: the previous "lenient behavior" was itself a spec violation (a bug);
> fixing it is a return to correctness, not an intentional change of product
> semantics. But every such change **must** be explicitly listed in the
> **⚠️ Breaking (security hardening)** section of the Release Notes, together
> with migration guidance.

This is an **explicit trade-off**, not a vague compromise. If the impact of a
particular hardening is assessed as genuinely broad (e.g. removing an entire
grant type), it should still go through the MAJOR + pre-release channel
(see §5).

---

## 4. Release Cadence

A **hybrid model**: periodic MINOR + on-demand PATCH + emergency security
hotfix.

| Event | Trigger |
|---|---|
| **Scheduled MINOR** (new features) | Every **4–6 weeks**; or when ≥ 3 `feat:` commits have accumulated |
| **PATCH** (bug fix) | When ≥ 5 `fix:` commits have accumulated; or when a user-reported bug has been fixed |
| **Emergency security PATCH** | **Immediately** after a P0 / CVE vulnerability fix (without waiting for the cadence) |
| **MAJOR** | When breaking changes have accumulated; must go through the pre-release channel (§5) |

The cadence is **guidance, not dogma**: skipping a cycle when nothing
release-worthy changed is perfectly fine; conversely, a P0 security fix always
ships immediately.

---

## 5. Pre-release Channel

Before an official MAJOR release, go through a staged pre-release ladder:

```
v2.0.0-alpha.1 → alpha.2 → … → v2.0.0-beta.1 → … → v2.0.0-rc.1 → … → v2.0.0
```

| Stage | Semantics | Accepted changes |
|---|---|---|
| `alpha.N` | Functionality may be incomplete; CI not guaranteed to pass | Anything (including new features, behavior adjustments) |
| `beta.N` | Feature freeze, feedback gathering | Bug fixes + non-breaking feedback-driven adjustments |
| `rc.N` | Release candidate | **Only** P0/P1 bug fixes |
| (suffix removed) | Official release | No new changes accepted |

**Image tag rules**:
- Official release → tagged `<version>` **and** `latest`
- pre-release → tagged **only** `<version>` (e.g. `v2.0.0-rc.1`), **not**
  `latest`

> **Current pipeline status**: the tag trigger pattern of `release.yml`,
> `v[0-9]+.[0-9]+.[0-9]+`, **accepts no suffix**, so pre-release tags currently
> do not trigger a release. Enabling the pre-release channel requires extending
> that regex to match `v[0-9]+.[0-9]+.[0-9]+(-[a-z]+\.[0-9]+)?` and, in the
> `github-release` job, deciding from whether the tag contains `-` whether to
> mark the GitHub Release as "Pre-release" and skip the `latest` manifest merge.
> This is a **pending pipeline change** of this policy (see §11).

---

## 6. DB Schema Versioning Is Decoupled from the Product Version

fulla uses numbered migrations (`V001_*` … `V0NN_*`) whose numbers increment
independently.

- **Additive migration (new table / new column / new index)** = backwards
  compatible → triggers a **MINOR** assessment.
- **Breaking migration (column drop / type change without backfill / rename)**
  → triggers a **MAJOR** assessment.
- The schema version table only records the migration application history and
  does **not** map to `MAJOR.MINOR.PATCH`.

---

## 7. Release Branch and Patch Release

After v1.2.0 is released, the mainline develops v1.3.0. If a P0 vulnerability
is found in v1.2.0:

```
master:  ──●──●──●──●──●──●──→  (developing v1.3.0)
               \
release/1.2:    └──●(cherry-pick fix)──● tag v1.2.1
```

**Conventions**:
- Branch naming: `release/<MAJOR>.<MINOR>` (e.g. `release/1.2`).
- The branch **accepts only cherry-picked bug fixes**, no new features.
- Each patch release cuts a `v<MAJOR>.<MINOR>.<PATCH>` tag, which triggers
  `release.yml`.
- **Maintenance window**: patches are maintained only for the **latest**
  release branch. The previous branch is EOL once a new minor is released
  (no LTS — see §8).

---

## 8. LTS (Long-Term Support)

**No LTS at the current stage.** Only the latest minor's patch releases are
maintained. Whether to introduce LTS (à la the Node.js / Kubernetes model) will
be reconsidered when the downstream user base grows and upgrade costs become
visible.

---

## 9. `latest` Tag Semantics and Production Deployment

- `:latest` points to the **latest official release** (pre-releases excluded).
- [`deploy/docker/docker-compose.prod.yml`](https://github.com/voidvec/fulla/blob/master/deploy/docker/docker-compose.prod.yml)
  uses `${FULLA_VERSION:-latest}`: a default for deployment convenience.
- ⚠️ **Production deployments should pin an explicit version number**
  (`FULLA_VERSION=1.2.0`) instead of relying on `latest` — it rolls
  uncontrollably whenever a new version is published.

---

## 10. Deprecation Process

Consistent with [SDK Runtime Contract](../sdk/sdk-runtime-contract.md) §2:

1. At the current MINOR release, annotate the deprecated API with
   `[[deprecated("Use X instead; removed in vN.0")]]`.
2. Record the deprecation + migration guidance in the **Deprecated** section of
   the Release Notes.
3. **Keep it for at least one MINOR cycle** (two recommended).
4. Remove it in the next MAJOR.

Non-SDK deprecations (config options, endpoint parameters) follow the same
"annotate → transition → remove" process, using Release Notes plus a
LOG_WARNING at config load time as the annotation mechanism.

---

## 11. Release Standard Operating Procedure (SOP)

### 11.1 Official MINOR / PATCH (from master)

```sh
# 1. Confirm master is green (CI fully passing)
git checkout master && git pull

# 2. Update the version number SSoT
#    Edit MINOR or PATCH in cmake/Version.cmake

# 3. Validate the API surface (critical step)
python3 tools/api-diff/api_diff.py
#   - additive drift (new headers / new declarations) → allowed for MINOR, ratify:
#       python3 tools/api-diff/api_diff.py --update-baseline
#   - breaking drift (removed / changed declarations) → MAJOR must be confirmed
#     bumped first; for changes that do not affect the consumed surface (private
#     members / include reordering, etc.), after review:
#       python3 tools/api-diff/api_diff.py --force --update-baseline

# 4. Generate a CHANGELOG draft, then curate manually
git cliff --unreleased --tag vX.Y.Z --prepend CHANGELOG.md
#   Manual editing essentials:
#     - Categorize into Added / Fixed / Changed / Security / Deprecated / ⚠️ Breaking
#     - Put security hardening into the ⚠️ Breaking (security hardening) section + migration guidance
#     - Drop entries with no information value

# 5. Commit the version number + baseline + CHANGELOG
git add cmake/Version.cmake tools/api-diff/*.baseline CHANGELOG.md
git commit -m "chore(release): vX.Y.Z"

# 6. Tag and push — triggers release.yml
git tag vX.Y.Z
git push origin master --tags
```

`release.yml` completes automatically: version-check → SDK tarball →
multi-arch images → cosign signing → SBOM → GitHub Release (with
git-cliff-generated notes + verification guidance).

### 11.2 Emergency Security PATCH (from a release branch)

```sh
# 1. Cherry-pick the fix commit onto the release/<MAJOR>.<MINOR> branch
git checkout release/1.2
git cherry-pick <fix-commit-sha>

# 2. Bump PATCH on that branch (same steps 2–6 as 11.1, but targeting the release branch)
```

### 11.3 MAJOR (via the pre-release channel)

```sh
# 1. Accumulate breaking changes on master (or a dedicated candidate branch)
# 2. Bump MAJOR, then tag prereleases in sequence:
git tag v2.0.0-alpha.1 && git push --tags   # → alpha stage
# ... feedback iterations ...
git tag v2.0.0-beta.1  && git push --tags   # → beta stage
git tag v2.0.0-rc.1    && git push --tags   # → rc stage (only P0/P1 fixes)
# 3. Once rc passes, drop the suffix for the official release:
git tag v2.0.0         && git push --tags
# 4. After the official release, create the release/2.0 branch
git checkout -b release/2.0 v2.0.0 && git push origin release/2.0
```

> ⚠️ As stated in §5: pre-release tags currently do **not trigger**
> `release.yml`. Complete the pipeline change (see the §12 backlog) before
> enabling this channel.

---

## 12. Backlog (Gaps Between This Policy and the Current State)

| # | Item | Problem it solves |
|---|---|---|
| **T3** | Extend the `release.yml` tag trigger pattern + `latest` skip logic, enabling the pre-release channel | Prerelease tags currently do not trigger a release |
| **T4** | Add a `latest` warning cross-reference to the prod deployment doc (`docker-deployment.md`) | The `latest` default poses a rolling-update risk in production |
| **T5** | Define the release branch naming convention and add a pointer in the README (create the branch when first actually needed) | The patch release process is not yet instantiated |

T1 (this document) and T2 (the first release after v1.0.0, shipped as v1.0.1)
are done and no longer listed; T3–T5 can land when their scenarios first occur.
