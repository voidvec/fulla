# RBAC Access Control (Role-Based Access Control)

fulla's access control is a **three-layer model** enforced by **three gates**
(since v1.6.0, P1-8, [ADR-0014](../adr/ADR-0014-rbac-permission-enforcement.md)
including its Erratum 2): roles are the coarse layer, OAuth2 resource scopes
are the fine layer, and a role's **permission set** — the bridge — is the
subset of resource scopes that ride on the role. This page documents the
model, the schema, the management APIs, how each gate decides, and how to
configure and operate it.

> **Read this first**: a role no longer just "is admin or isn't". The `admin`
> role still passes everything, but any custom role can be granted a subset
> of resource-scope permissions (delegated administration — e.g. a helpdesk
> role that may only read users). Conversely, holding a role never bypasses
> the token-scope or consent tiers: a permission grants the *right to hold a
> scope*, the token must still carry it.

## 1. Core Concepts

| Layer | What it is | Examples | Lives in |
|---|---|---|---|
| **Role** (coarse) | A named principal grouping attached to users (M:N) | `admin`, `user`, `helpdesk` | `roles` / `user_roles` |
| **Resource scope** (fine) | The OAuth2 scope vocabulary; the fine-grained authorization surface | `users:read`, `clients:write`, `audit:read` | `oauth2_scopes` (V023 family) + `ResourceScopeRegistry` |
| **Permission** (the bridge) | A role's right to hold one resource scope — a permission's name IS a resource-scope name | `users:read` on the `helpdesk` role | `permissions` / `role_permissions` |

Relationship model:

- `User <-> Role`: many-to-many (`user_roles`); assigned at registration
  (default `user` role) and via `PUT /api/admin/users/{userId}/roles`.
- `Role <-> Permission`: many-to-many (`role_permissions`); the role's
  resource-scope set, managed via `PUT /api/admin/roles/{roleId}/permissions`.
- The special permission **`admin`** is a super-permission: the `admin` role
  is seeded with it plus every resource scope, and the literal `admin` role
  passes all three gates regardless of any grant (chain ① keeps
  `hasAdminRole` as a separate bypass).

## 2. Database Schema (V005 + V043)

`V005__rbac_schema.sql` creates the four RBAC tables:

```sql
roles              (id, name UNIQUE, description, ...)          -- seeded: 'admin', 'user'
permissions        (id, name UNIQUE, description, ...)
user_roles         (user_id FK users,  role_id FK roles,  PK(user_id, role_id))
role_permissions   (role_id FK roles, permission_id FK permissions, PK(role_id, permission_id))
```

`V043__rbac_permissions_seed.sql` (v1.6.0) seeds the permission catalog with
the nine V023 resource-scope names plus `admin`, and grants the `admin` role
the full set:

| Permission | Description (seed) |
|---|---|
| `users:read` | Read user accounts and roles |
| `users:write` | Create, update, and delete users; assign roles |
| `clients:read` | View OAuth2 client registrations |
| `clients:write` | Create, update, and delete OAuth2 clients |
| `tokens:read` | View issued tokens |
| `tokens:write` | Revoke and manage tokens |
| `roles:read` | View roles and the scope catalog |
| `roles:write` | Create and update roles, scopes, and their mappings |
| `audit:read` | Read and export audit logs |
| `admin` | Administrative super-permission (implies the full set) |

Notes:

- The four legacy V005 names (`user:read`, `user:write`, `user:delete`,
  `admin:access`) are **retired in place** — V043 rewrites their
  descriptions with a `[LEGACY: ...]` marker (UPDATE, no DELETE). The
  enforcement layer reads only V023-shaped names, so a legacy row (e.g. the
  old `user`-role `user:read` grant) is inert by design; re-grant with the
  new names.
- **Memory mode** (`storage_type: memory`) holds no `role_permissions` rows;
  `RolePermissionResolver` falls back to a compile-time default map that
  mirrors the V043 seed: `admin` → the full ten-permission set, every other
  role → none.

## 3. Management APIs

All routes below are admin-gated (`roles:read` / `roles:write` /
`users:write` scope, each with `impliedBy: admin`) —
[API Reference](api-reference.md):

| Method & path | Purpose |
|---|---|
| `GET /api/admin/roles` | List roles with user counts (`roles:read`) |
| `POST /api/admin/roles` | Create a role — built-ins (`admin`, `user`) cannot be duplicated (`roles:write`) |
| `PUT /api/admin/roles/{roleId}` | Update a role's description (`roles:write`) |
| `DELETE /api/admin/roles/{roleId}` | Delete a role — built-ins cannot be deleted (`roles:write`) |
| `GET /api/admin/roles/{roleId}/permissions` | The role's permission-name list (`roles:read`) |
| `PUT /api/admin/roles/{roleId}/permissions` | **Full-replace** the role's permission set (`roles:write`) |
| `GET /api/admin/users/{userId}/roles` | Roles assigned to a user (`users:read`) |
| `PUT /api/admin/users/{userId}/roles` | Assign a user's roles — 409 if it would strip the last active admin (`users:write`) |
| `GET /api/admin/scopes` | The OAuth2 scope catalog (`roles:read`) |
| `GET /api/admin/scopes/resources` | The (path, method) → required-scopes matrix (see §5) |

**PUT permissions semantics**: the body's `permissions` array becomes the
role's entire grant set. Every listed name must exist in the permissions
catalog — one unknown name fails the whole replace with 400 (no partial
grants). The change is transactional, audited as `role_permissions_updated`,
and the role-permission cache entry is invalidated on commit (single-instance
deployments are immediately consistent; multi-instance drift is a known
limitation until shared-storage P3).

**First admin**: with `auth.bootstrap_admin.enabled` (default `true`) the
server creates the `admin` user at startup when no user holds the admin role
— password from `FULLA_BOOTSTRAP_ADMIN_PASSWORD` or a random one printed to
the log exactly once (see
[Configuration Guide §2](../operate/configuration-guide.md)). Manual SQL is
no longer needed for either the first admin or permission grants.

Example — grant a helpdesk role read-only user access:

```bash
# 1. Create the role (token: admin scope)
curl -X POST http://localhost:5555/api/admin/roles \
  -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
  -d '{"name":"helpdesk","description":"Read-only user support"}'

# 2. Grant exactly one permission (full replace)
curl -X PUT http://localhost:5555/api/admin/roles/3/permissions \
  -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
  -d '{"permissions":["users:read"]}'

# 3. Attach it to a user (full role-list replace)
curl -X PUT http://localhost:5555/api/admin/users/5/roles \
  -H "Authorization: Bearer $ADMIN_TOKEN" -H "Content-Type: application/json" \
  -d '{"roles":["user","helpdesk"]}'
```

## 4. Enforcement — the Three Gates

Authorization decisions are made in three independent places. All three were
re-pointed to the permission lookup in v1.6.0; skipping any one of them
would give delegated admins inconsistent verdicts depending on the request
path.

| Gate | Component | Decides |
|---|---|---|
| ① Scope tiering | `ScopeDecisionEngine` predicate chain (libs/oauth2) | Whether a **requested scope** may be issued / used by this user |
| ② Identity checks | `IdentityService::validateUserRolesForScopes` (libs/drogon services) | Same question on the login/2-step path (and any direct callers) |
| ③ HTTP role gate | `AuthorizationFilter::checkAccess` (libs/drogon filters) | Whether this **HTTP request** may proceed at all |

**Gate ① — scope tiering (issuance)**. At `/oauth2/authorize` (and token
exchange), each requested scope walks three tiers: client allowlist →
admin-tier check → consent. Which scopes are *admin-class* comes from the
DB (`oauth2_scopes.requires_admin_role`, loaded at startup by `OAuth2Plugin`
into `AuthorizationService` and `IdentityService` — deprecated column, read
through v2.0). Whether the user *passes* is permission-based: an admin-class
scope passes when the user's role→permission union covers it, **or** the
user holds the literal `admin` role. Classification and determination are
deliberately decoupled — granting a permission never de-classifies a scope.

**Gate ② — IdentityService**. The login path (`/oauth2/login` and the
2-step portal flow) tiers requested scopes through the same logic before
minting an authorization code: admin-class scopes require the literal
`admin` role or a permission union that covers **every** requested
admin-class scope.

**Gate ③ — AuthorizationFilter (per request)**. On every admin route the
filter:

1. Extracts the token from the `Authorization: Bearer` header **only** (the
   query/form fallback was removed in v1.6.0 — RFC 9700 forbids it).
2. Validates the access token (opaque reference tokens are introspected
   against storage) and pins the principal (`userId`, `scope`, `clientId`,
   `orgId`) onto the request.
3. **Scope gate first**: looks the route up in
   `ResourceScopeRegistry` — the central (path, method) → required-scopes
   matrix — and rejects with `403 insufficient_scope` (+ RFC 6750
   `WWW-Authenticate` challenge) unless the token's scope set satisfies it.
4. **Role/permission gate**: re-resolves the user's roles from the DB
   (roles in the token response are informational — never trusted for
   access decisions), resolves the role→permission union via
   `RolePermissionResolver`, and matches the request path against
   `rbac_rules` (§5). Failure → `403` `AUTHZ_INSUFFICIENT_PERMISSIONS`;
   no rule matched and not a public path → **DEFAULT DENY**.

## 5. The Scope System: OIDC Scopes vs Resource Scopes

Two scope families coexist in `oauth2_scopes`:

- **OIDC / identity scopes** — `openid`, `profile`, `email` (plus `org`,
  v1.4.0): describe what identity data a client may see. Granted by user
  consent; never admin-class.
- **Resource scopes** — the V023 family (`users:read` … `audit:read`,
  all `requires_admin_role = TRUE`): gate the admin API surface, one
  (path, method) entry per route in `ResourceScopeRegistry`.

Implication rules that matter in practice:

- A token carrying **`admin`** satisfies any resource-scope requirement —
  every admin route declares `impliedBy: ["admin"]` (implication is
  per-route, not a global graph).
- `admin` **never** implies `openid` / `profile` / `email` — those
  requirements carry an empty `impliedBy`, so a userinfo call always needs
  an actual user token with `openid`. Likewise the `/api/me` family
  requires `profile` with **no** impliedBy: an admin token does not satisfy
  user self-service.
- A role's permission set says which resource scopes a user **may be
  issued**; the token must still carry the scope (client requests it,
  gates ①/② validate it, consent is recorded).

Discover the live matrix (read-only, no DB access) with:

```bash
curl http://localhost:5555/api/admin/scopes/resources \
  -H "Authorization: Bearer $ADMIN_TOKEN"
# → {"resources":[{"path":"/api/admin/users","method":"GET",
#    "required_scopes":["users:read"],"implied_by":["admin"]}, ...]}
```

The registry is built once at startup from every controller's OpenAPI
endpoint declarations; a startup consistency check aborts the process if an
admin-gated route lacks an entry (silent-gap protection).

## 6. Delegated Administrators (v1.6.0)

The three-gate model unlocks what the pre-v1.6 "literal admin role" gates
could not express: **custom roles with a limited permission subset**.
Worked example — the `helpdesk` role from §3, holding only `users:read`:

- `GET /api/admin/users` works: gate ③'s `/api/admin/users.*` rule
  (`{"permissions":["users:read"]}`) matches before the `/api/admin/.*`
  catch-all; gate ① lets helpdesk users be issued `users:read` (the token
  must carry it and consent must be recorded).
- Everything else admin-side is denied: the catch-all
  `"/api/admin/.*": ["admin"]` requires the literal role, and gates ①/②
  reject other admin-class scopes (no permission covers them).
- Red line (security review H2): a helpdesk grant never *de-classifies* a
  scope — `users:write` stays admin-class; helpdesk simply fails the
  permission check for it.

Operational notes:

- Permission and role-permission writes route through the admin API, which
  invalidates the in-process role-permission cache on commit — single-node
  deployments see changes immediately; on multi-instance deployments other
  nodes serve stale grants until restarted (known limitation, resolved by
  the P3 shared-storage work package).
- Memory-mode deployments use the compile-time default map (§2) and cannot
  express delegated roles.

## 7. Configuration Quick Reference

`rbac_rules` lives in `config.json` under `custom_config` and maps **regex
path patterns** to requirements. The shipped default
(`apps/server/config.json`) shows both forms:

```json
"rbac_rules": {
    "/api/admin/users.*": { "permissions": ["users:read"] },
    "/api/admin/.*": ["admin"],
    "/api/user/.*": ["user", "admin"],
    "/oauth2/register": ["admin"],
    "/oauth2/device/approve": ["admin"]
}
```

- **Array form** (legacy, unchanged): OR over role names — any one listed
  role passes.
- **Object form** `{"roles": [...], "permissions": [...]}` (v1.6.0): the
  caller's role→permission union must cover **every** listed permission
  name; a permissions-only entry is the delegated-administration shape.
- **Matching**: most-specific-first — rules are sorted by descending
  pattern length at load, and the **first** path match decides. Put
  specific rules before catch-alls (the loader enforces this, but keep the
  JSON readable in that order too).
- **DEFAULT DENY**: a path that matches no rule and is not in
  `public_paths` (optional regex array, unset by default) is rejected.
- The shipped `/api/user/.*` entry is a legacy remnant — no registered
  route lives under `/api/user/` (the user-facing family is `/api/me/*`,
  which is scope-gated by `profile` via `OAuth2AuthFilter`, not by
  rbac_rules). Harmless, but don't copy it as a template.

Related reading: [Configuration Guide §15](../operate/configuration-guide.md)
(RBAC enforcement knobs), [Multi-Tenancy](multi-tenancy.md) (why roles are
global, not per-org), [Architecture Overview](../architecture/architecture-overview.md).
