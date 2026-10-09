---
sidebar_position: 7
---

# Multi-Tenancy (Organizations)

fulla's multi-tenancy today is an **organizational layer**: organizations
group users and clients, carry branding fields, anchor the v1.4.0 tenant
semantics (org-scoped authorization, organization consents, owner
succession), and are managed through admin APIs plus self-service portal
flows. This page documents exactly what exists, what it does **not**
do yet, and how to use it without over-assuming isolation.

> **Read this first**: organizations are metadata and ownership grouping,
> not a hard isolation boundary. Authorization is enforced by RBAC + scopes
> (see [RBAC Guide](rbac-guide.md)); today an org-scoped principal is not
> automatically fenced off from other orgs' data.

## 1. Model (V017 + V034/V036–V038)

The core `organizations` table is V017; the organization layer now spans four
more migrations — `organization_members` + `organization_invitations` (V034),
`organization_consents` (V036; the same migration adds
`organizations.require_mfa`, org columns on codes/tokens, `audit_logs.org_id`,
and seeds the `org` scope), `organization_succession_nominations` (V037),
and `organization_consent_requests` (V038):

```sql
organizations (
    id              SERIAL PRIMARY KEY,
    slug            VARCHAR(50) UNIQUE,   -- 3–50 chars, lowercase
    name            VARCHAR(200),
    logo_uri        VARCHAR(512),         -- branding
    primary_color   VARCHAR(7),           -- branding
    issuer_override VARCHAR(512),         -- stored; see §4 roadmap
    require_mfa     BOOLEAN DEFAULT FALSE,-- V036; enforced by the org context gate
    created_at / updated_at
)
```

Two nullable foreign keys attach entities to an organization:

| Column | On | Semantics |
|---|---|---|
| `org_id` | `users` | Deprecated single-org anchor from V017. **Read-only since v1.4.0** (the admin API write surface was removed); `NULL` = unset. Physical removal planned for v2.0. |
| (dropped) | `oauth2_clients` | The V017 `org_id` column was never read or written by code and was **dropped in V036**. Client ownership lives in `oauth2_client_owners.org_id` (v1.4.0). |

The authoritative organization anchors since v1.4.0 are the M:N membership
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
| `POST /api/admin/organizations/{slug}/transfer-ownership` | Reassign the owner seat to another live user (rescues the soft-deleted-owner deadlock; the old owner is demoted to admin) |

Additionally:

- The admin user API (`POST/PUT /api/admin/users`) **no longer accepts
  `org_id`** (v1.4.0 org-anchor convergence): a request body containing the
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
- **Organization context in the authorization chain (v1.4.0)**: the authorize
  endpoint accepts an optional `org_id` (id or slug) validated by the org
  context gate — requester must be a live member, the client must belong to
  that org or hold an active organization consent, and an org with
  `require_mfa` enforced rejects password sessions.
  The binding flows through the code-to-token chain, and tokens issued with
  the `org` scope carry **`org_ctx` claims** (userinfo / id_token) that drop
  immediately when membership ends.
- **Organization consents (v1.4.0)**: when a member authorizes an org-owned
  application, the grant is recorded on the **organization** (admin-level
  rows in `organization_consents`); members can review and revoke grants on
  the portal's "Org authorizations" page
  (`GET /api/me/organizations/{slug}/consents`,
  `DELETE /api/me/organizations/{slug}/consents/{clientId}`).
- **Consent request workflow (v1.4.0, #236)**: members who are not owners can
  file an organization-authorization **request**
  (`organization_consent_requests`); managers approve/reject from the
  portal, the requesting member can withdraw their own pending request, and
  an approved request becomes the org grant.
- **Owner succession (v1.4.0)**: an owner can nominate a successor
  (nominate → accept two-step confirmation), a soft-deleted owner's seat is
  auto-succeeded from a pending nomination, and the admin transfer-ownership
  endpoint above rescues orgs with no eligible successor.
- **Organization-anchored client credentials (v1.4.0)**: client_credentials
  tokens for org-owned applications carry the `org_id` claim (the subject
  stays the client id).
- **No migration cliff**: everything is optional and additive; deployments
  that don't care about orgs never touch it.

## 4. What it does NOT do yet (roadmap)

Be explicit with stakeholders — these are **not** implemented:

1. **`issuer_override` is stored but not applied**: per-org issuer in the
   discovery document and in issued tokens is schema-ready, not runtime-live.
2. **No org-scoped filtering/isolation** on user or client listings; an
   admin sees across orgs.
3. **No org-scoped roles**: roles are global (RBAC), not per-org.
4. **No update/delete** endpoints for organizations (create/list/get +
   ownership transfer only; updates go through the database or future API).
5. **Per-org rate limits and API keys don't exist yet** — but creation
   *quotas* do: self-service org creation is quota'd per user
   (`max_orgs_per_user`), org-owned client counts are quota'd
   (`max_org_apps`), and self-service *application* creation is rate limited
   (`creation_rate_limit_per_day`) (open-platform limits).

If you need hard tenant isolation today, run one fulla stack per tenant —
the Docker Compose / Helm paths make that cheap
([Deployment](../operate/deployment.md)).

## 5. Schema reference

The organization layer spans several migrations — the authoritative DDL lives
in [`apps/server/migrations/`](https://github.com/voidvec/fulla/tree/master/apps/server/migrations):

| Migration | Content |
|---|---|
| `V017__multi_tenant.sql` | `organizations` core table (indexed `users(org_id)`, `organizations(slug)`) |
| `V034` | `organization_members` / `organization_invitations` |
| `V036` | `organization_consents` + `organizations.require_mfa` + org columns on the authorization chain + dropped the dead `oauth2_clients.org_id` |
| `V037` | `organization_succession_nominations` |
| `V038` | `organization_consent_requests` |

`users.org_id` is a legacy read-only column slated for v2.0 physical removal.
Storage-layer details: [Data Persistence](../architecture/data-persistence.md).
