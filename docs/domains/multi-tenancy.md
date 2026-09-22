---
sidebar_position: 7
---

# Multi-Tenancy (Organizations)

fulla's multi-tenancy today is an **organizational layer**: organizations
group users and clients, carry branding fields, and are managed through
admin APIs. This page documents exactly what exists, what it does **not**
do yet, and how to use it without over-assuming isolation.

> **Read this first**: organizations are metadata and ownership grouping,
> not a hard isolation boundary. Authorization is enforced by RBAC + scopes
> (see [RBAC Guide](rbac-guide.md)); today an org-scoped principal is not
> automatically fenced off from other orgs' data.

## 1. Model (V017)

```sql
organizations (
    id              SERIAL PRIMARY KEY,
    slug            VARCHAR(50) UNIQUE,   -- 3–50 chars, lowercase
    name            VARCHAR(200),
    logo_uri        VARCHAR(512),         -- branding
    primary_color   VARCHAR(7),           -- branding
    issuer_override VARCHAR(512),         -- stored; see §4 roadmap
    created_at / updated_at
)
```

Two nullable foreign keys attach entities to an organization:

| Column | On | Semantics |
|---|---|---|
| `org_id` | `users` | Deprecated single-org anchor from V017. **Read-only since v1.5.0** (the admin API write surface was removed); `NULL` = unset. Physical removal planned for v2.0. |
| (dropped) | `oauth2_clients` | The V017 `org_id` column was never read or written by code and was **dropped in V036**. Client ownership lives in `oauth2_client_owners.org_id` (v1.4.0). |

The authoritative organization anchors since v1.5.0 are the M:N membership
table (`organization_members`) and `oauth2_client_owners.org_id`;
`users.org_id` is a legacy read-only column kept only for migration.

## 2. Admin API surface

All routes require an admin-scope token (`AuthorizationFilter`;
`impliedBy: admin`) — [API Reference](api-reference.md) §client management:

| Method & path | Purpose |
|---|---|
| `GET /api/admin/organizations` | List organizations (id, slug, name, branding) |
| `POST /api/admin/organizations` | Create (slug: 3–50 lowercase chars, unique) |
| `GET /api/admin/organizations/{slug}` | Fetch one |

Additionally:

- The admin user API (`POST/PUT /api/admin/users`) **no longer accepts
  `org_id`** (v1.5.0 org-anchor convergence): a request body containing the
  key is rejected with 400, whatever its value. Organization membership is
  managed through the organization membership APIs (`organization_members`),
  and the `users.org_id` column stays readable until its v2.0 physical drop.

Example:

```bash
# Create an organization (token: admin scope)
curl -X POST http://localhost:5555/api/admin/organizations \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"slug":"acme","name":"ACME Corp","logo_uri":"https://acme.example/logo.svg","primary_color":"#5b2fd1"}'
```

## 3. What this buys you today

- **Ownership bookkeeping**: which human belongs to which company, which
  client application belongs to which company — queryable via the admin API
  and SQL (membership tables; `users.org_id` is a deprecated read-only
  legacy column).
- **Branding catalog**: per-org logo and primary color for frontends that
  want to skin the login experience per tenant.
- **No migration cliff**: everything is optional and additive; deployments
  that don't care about orgs never touch it.

## 4. What it does NOT do yet (roadmap)

Be explicit with stakeholders — these are **not** implemented:

1. **`issuer_override` is stored but not applied**: per-org issuer in the
   discovery document and in issued tokens is schema-ready, not runtime-live.
2. **No org-scoped filtering/isolation** on user or client listings; an
   admin sees across orgs.
3. **No org-scoped roles**: roles are global (RBAC), not per-org.
4. **No update/delete** endpoints for organizations (create/list/get only).
5. **No per-org rate limits, quotas, or keys**.

If you need hard tenant isolation today, run one fulla stack per tenant —
the Docker Compose / Helm paths make that cheap
([Deployment](../operate/deployment.md)).

## 5. Schema reference

The authoritative DDL is
[`V017__multi_tenant.sql`](https://github.com/voidvec/fulla/blob/master/apps/server/migrations/V017__multi_tenant.sql)
(V017 indexed `users(org_id)`, `oauth2_clients(org_id)`, and
`organizations(slug)`; the `oauth2_clients.org_id` column and its index were
dropped in V036, and `users.org_id` is slated for v2.0).
Storage-layer details: [Data Persistence](../architecture/data-persistence.md).
