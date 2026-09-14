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
| `org_id` | `users` | The user belongs to the org; `NULL` = unassigned (backwards compatible) |
| `org_id` | `oauth2_clients` | The client is owned by the org; `NULL` = global/ownerless |

Both are nullable by design: pre-V017 data and platform-level principals
(the seed `admin`, the `fulla-admin-console` client) simply have no org.

## 2. Admin API surface

All routes require an admin-scope token (`AuthorizationFilter`;
`impliedBy: admin`) — [API Reference](api-reference.md) §client management:

| Method & path | Purpose |
|---|---|
| `GET /api/admin/organizations` | List organizations (id, slug, name, branding) |
| `POST /api/admin/organizations` | Create (slug: 3–50 lowercase chars, unique) |
| `GET /api/admin/organizations/{slug}` | Fetch one |

Additionally:

- `POST/PATCH /api/admin/users` accepts `org_id` — an integer assigns the
  user to an org; JSON `null` **clears** the assignment.

Example:

```bash
# Create an organization (token: admin scope)
curl -X POST http://localhost:5555/api/admin/organizations \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"slug":"acme","name":"ACME Corp","logo_uri":"https://acme.example/logo.svg","primary_color":"#5b2fd1"}'

# Attach a user to it
curl -X PATCH http://localhost:5555/api/admin/users/42 \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"org_id": 1}'
```

## 3. What this buys you today

- **Ownership bookkeeping**: which human belongs to which company, which
  client application belongs to which company — queryable via the admin API
  and SQL (`users.org_id`, `oauth2_clients.org_id`).
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
(indexes on `users(org_id)`, `oauth2_clients(org_id)`, `organizations(slug)`).
Storage-layer details: [Data Persistence](../architecture/data-persistence.md).
