---
adr: 0014
title: RBAC Permission Enforcement — Permissions as a Role's Resource-Scope Set, Replacing Both Admin-Scope Gates
date: 2026-10-11
status: Accepted
---

## Context

The RBAC schema (`V005`: `roles` / `permissions` / `user_roles` / `role_permissions`) ships with seed data but has **zero enforcement** — no business code reads `role_permissions` to make an authorization decision (only drogon_ctl-generated ORM boilerplate references it). `docs/domains/rbac-guide.md:11` self-describes the model as "simplified to role-based URL interception". The 2026-10 IAM gap analysis called this an architectural misleading (evaluators read the schema and assume a working permission engine).

What actually gates admin operations today are **two parallel, independent chains**:

1. **DB-column chain**: `OAuth2Plugin` startup loads the `oauth2_scopes.requires_admin_role` column into an injected predicate (`OAuth2Plugin.cc:566-585`); `ScopeDecisionEngine.cc:31-36` consumes it (`scopeRequiresAdmin(scope) && !hasAdminRole → admin_role_required`). The engine itself never reads the column, and `AuthorizationService.cc:42-56` keeps a hardcoded `defaultAdminScopes()` fallback set used only when the DB has none.
2. **Hardcoded chain**: `IdentityService::scopeRequiresAdminRole` (`IdentityService.cc:227-240`) writes a literal set `{admin, admin:read, admin:write, user:manage, settings:manage}` (with prefix matching), consumed by `validateUserRolesForScopes` (`:188-224`) and forwarded from `OAuth2Plugin.cc:1254-1256`.

Because both chains require the literal `admin` **role**, a delegated administrator — e.g. a "helpdesk" role that may only read users — cannot be expressed at all. Decision D2 (2026-10-10, roadmap `deliverables/iam-improvement-roadmap-2026-10.md` §8) chose **enforcement over freeze/remove**, with an "integrate, don't duplicate" constraint.

## Decision

1. **Semantics**: a `permissions` row is re-purposed as **the right of a role to hold a resource scope**. A permission's name IS a resource scope name (`users:read`, `clients:write`, `audit:read`, …) — the vocabulary already seeded by `V023__resource_scopes.sql`. No new permission grammar is invented; the fine-grained authorization surface remains the OAuth2 scope model (scope catalog + `ResourceScopeRegistry` (path,method)→scope + consent), exactly as today. `role_permissions` becomes "which resource scopes ride on this role".
2. **Integration — both chains, not one**: the P1 implementation replaces the admin check in **each** chain with the same lookup:
   - Chain 1: the predicate injected into `ScopeDecisionEngine` becomes "does the user hold, via ANY of their roles, a permission whose name equals the requested scope (or `admin`, which remains a super-scope implying every resource scope)".
   - Chain 2: `IdentityService::scopeRequiresAdminRole` + its literal set are deleted; `validateUserRolesForScopes` consults the same permission lookup. Skipping chain 2 would leave delegated admins with inconsistent verdicts depending on which path evaluated the request (the failure mode the 2026-10-10 review flagged).
3. **Compatibility seed**: a migration grants the `admin` role every resource scope present in the catalog, so admin behavior is byte-identical before/after; the `requires_admin_role` column and the hardcoded set are both marked deprecated (column dropped no earlier than v2.0 after a deprecation window).
4. **Unlocked capability — delegated administrators**: custom roles can be granted a subset (e.g. helpdesk = `users:read` only). The admin console's Roles page gains a role→scope-set assignment UI. Acceptance (P1-8): a fresh "helpdesk" role with only `users:read` can list users and nothing else; the `admin` role behaves exactly as before.
5. **api-diff constraint**: `IMfaRepository`-style interface widening is not permitted in a minor; the lookup lives behind the existing `IRoleProvider`/`IAuthorizationService` seams (extend the **concrete** `StorageRoleProvider` / `AuthorizationService`, or add a new small port in `common` if a new seam is unavoidable — additive only).

## Non-goals (explicitly out of scope for P1-8)

- Role hierarchies, role→client bindings, and org-scoped roles (revisit after P1-8 lands; roadmap §11 I12).
- ABAC / a general policy engine.
- Changing what `ResourceScopeRegistry` maps or how consent works.

## Consequences

- `role_permissions` stops being dead schema and becomes the delegated-admin mechanism; `rbac-guide.md` is rewritten to the three-layer model (role = coarse, scope = fine, role→scope-set = the bridge).
- The two deprecated gates (`requires_admin_role` column, `IdentityService` literal set) shrink to no-op shims for one release, then are removed.
- Implementation is tracked as roadmap work package P1-8 (v1.6.0); this ADR is the design contract that issue/PR references.
