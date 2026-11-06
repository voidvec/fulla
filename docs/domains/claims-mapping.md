---
sidebar_position: 8
---

# Custom Claims (Claims Mapping)

Since **v1.6.0** (P1-3, #280) fulla can publish **per-user custom claims** into
`id_token` and `userinfo`: users maintain a self-service
`custom_attributes` document, and an admin maps a source path to a published
claim under a **trigger scope** — the minimal equivalent of Keycloak's protocol
mappers. This page documents the model, the two API surfaces, the security red
lines (and why they exist), the publication semantics, and the cache behavior.

> **Read this first**: this feature is deliberately narrow. Mappings can only
> read `custom_attributes.<key>` or the built-in `user.display_name`, can never
> shadow a standard claim, and are gated on a scope the RP explicitly requests.
> If you are looking for arbitrary claim transformation or federation
> protocol-mappers, that is out of scope by design (see §7).

## 1. Model (V044)

One migration ships the feature — `V044__custom_attributes_claim_mappings.sql`:

```sql
-- user-side source data (self-service, full-replace)
ALTER TABLE users ADD COLUMN IF NOT EXISTS custom_attributes JSONB
    NOT NULL DEFAULT '{}';
CREATE INDEX idx_users_custom_attributes_gin ON users USING GIN (custom_attributes);

-- admin-side publication rules
CREATE TABLE claim_mappings (
    id SERIAL PRIMARY KEY,
    scope_name  VARCHAR(64)  NOT NULL,          -- trigger scope (NOT 'openid')
    claim_name  VARCHAR(64)  NOT NULL,          -- published claim name
    source_path VARCHAR(128) NOT NULL,          -- custom_attributes.<key> | user.display_name
    include_in  VARCHAR(16)  NOT NULL DEFAULT 'both',  -- id_token | userinfo | both
    enabled     BOOLEAN      NOT NULL DEFAULT TRUE,
    CONSTRAINT uq_claim_mappings UNIQUE (scope_name, claim_name)
);
```

A mapping is a single rule: *when a token/userinfo request carries
`scope_name`, publish the value found at `source_path` as the claim
`claim_name`, in the surfaces selected by `include_in`*. One claim name per
scope is enforced by the unique constraint; the same claim under a different
scope is a separate row.

Postgres only — there is no in-memory claim-mapping store: the self-service
surface answers **404 on memory-storage deployments** (custom attributes are
claims-mapping source data, not profile fields), and the admin CRUD requires
the database client, answering `DB_CONNECTION_ERROR` without one.

## 2. Self-service: the custom-attributes document

| Endpoint | Purpose |
|---|---|
| `GET /api/me/profile/custom-attributes` | Return the caller's document (`{status, custom_attributes}`). |
| `PUT /api/me/profile/custom-attributes` | **Full-replace** the document (no merge). |

Write-time validation (`CUSTOM_ATTRIBUTES_INVALID`, error 3026, HTTP 400):

- must be a JSON **object** (not an array/scalar);
- **depth ≤ 2** (the document root plus one nesting level);
- serialized size **≤ 8 KiB**;
- every key matches `^[a-z][a-z0-9_]{0,63}$`;
- the prototype-pollution trio (`__proto__` / `constructor` / `prototype`) is
  rejected outright (the regex already excludes them; the explicit check
  documents the intent and guards regex regressions).

```bash
curl -X PUT https://fulla.example.com/api/me/profile/custom-attributes \
  -H "Authorization: Bearer $USER_ACCESS_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"department": "platform", "employee_id": "E-4102", "prefs": {"locale": "zh-CN"}}'
```

Full-replace semantics mean the client should GET, mutate, then PUT the whole
document. Values may be any JSON type, but **only scalars ever publish** as
claims (§5) — objects/arrays are legal to store, never emitted.

## 3. Admin API: claim mappings

Admin-scoped (`GET/POST /api/admin/claim-mappings`, `PUT/DELETE
/api/admin/claim-mappings/{mappingId}`); there is intentionally **no admin
console page yet** — the CRUD is API-only.

| Operation | Endpoint | Notes |
|---|---|---|
| List | `GET /api/admin/claim-mappings` | `{status, total, mappings[]}` with all five fields. |
| Create | `POST /api/admin/claim-mappings` | Body: `scope_name`, `claim_name`, `source_path` (required), `include_in` (`id_token` \| `userinfo` \| `both`, default `both`). 201. |
| Update | `PUT .../{mappingId}` | Full body, plus `enabled` for activation toggling. 404 on unknown id. |
| Delete | `DELETE .../{mappingId}` | 404 on unknown id. |

```bash
curl -X POST https://fulla.example.com/api/admin/claim-mappings \
  -H "Authorization: Bearer $ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"scope_name": "hr", "claim_name": "employee_id",
       "source_path": "custom_attributes.employee_id", "include_in": "both"}'
```

Rejections are structured VALIDATION errors (HTTP 400):

| Error | Code | Meaning |
|---|---|---|
| `CLAIM_MAPPING_RESERVED_CLAIM` | 3023 | `claim_name` is on the reserved blacklist (§4). |
| `CLAIM_MAPPING_FORBIDDEN_SCOPE` | 3025 | `scope_name` is `openid`. |
| `CLAIM_MAPPING_INVALID_SOURCE` | 3024 | `source_path` is outside the whitelist (§4). |

## 4. Security red lines — and why

The design's adversarial review found an **account-takeover chain** in the
draft (an admin mapping onto `email`/`sub` could rewrite the identity
assertions RPs trust). The shipped closures, enforced at **write time** on
every mapping create/update:

1. **Reserved-claim blacklist** — `claim_name` may never shadow a claim the
   server already emits or OIDC reserves:
   `sub, iss, aud, exp, iat, nonce, auth_time, acr, amr, org_ctx, email,
   email_verified, name, preferred_username, family_name, given_name,
   nickname, picture, website, phone_number, phone_number_verified,
   username, roles`.
   `claim_name` must additionally match `^[a-z][a-z0-9_]{0,63}$`.
2. **Source whitelist** — exactly `custom_attributes.<key>` (key bound to the
   self-service grammar above) and the single built-in `user.display_name`.
   The draft's `org.role` source was **cut**: publishing role-ish data through
   a user-editable channel is a cross-context leak.
3. **No `openid` trigger** — `openid` is unconditionally granted, so a mapping
   on it would be zero-authorization publication. Choose a scope the RP must
   actually request (a catalog scope or a custom one).

## 5. Publication semantics

- **Trigger-scope gate**: a mapping fires only when the request's scope set
  contains `scope_name` (exact match, standard OAuth2 scope-token semantics).
  A bare `openid` token publishes nothing.
- **Surfaces** (`include_in`): `id_token` (issued on the authorization-code
  path **and** re-evaluated at every refresh rotation — rotating a legacy
  refresh family re-computes mapped claims), `userinfo`, or `both`.
- **Scalars only**: source resolution publishes a value only when the path
  exists **and** resolves to a scalar. Absent path, object, or array → the
  claim is simply omitted for that user (never an error).
- **Discovery**: enabled mappings' claim names are advertised in
  `claims_supported` on both metadata endpoints (`.well-known/
  openid-configuration` and `oauth-authorization-server`), refreshed through
  the same cache hook as the rest of the catalog — see §6.

Standard scope gating still applies underneath (v1.5.0): `name` /
`preferred_username` / `picture` need `profile`, `email` / `email_verified`
need `email`. Mapped claims add to, never relax, that behavior.

## 6. Cache behavior (hot-path safety)

The first implementation re-queried `claim_mappings` on **every** userinfo and
issuance call — the v1.6.0 five-scenario benchmark gate measured it costing
17–18% QPS on the affected scenarios versus v1.5.0. Shipped fix:

- `StorageClaimMappingsProvider` is wrapped in `CachedClaimMappingsProvider`
  (process-local cache). The **TTL only bounds cross-process drift** — every
  claim-mapping write path invalidates the cache immediately, and the same
  hook refreshes discovery's `claims_supported`, so CRUD is visible at once
  (verified by the `ClaimsMapping_PublicationChain` integration test:
  create → next token/userinfo carries the claim; delete → next call drops it).

Introspection's organization overlay got the same treatment in the same
release (3s → 60s TTL plugin cache, invalidated from the two owner-row write
paths).

## 7. Limits (by design)

- No transformation, concatenation, or multi-value mapping — a source path is
  copied verbatim when it is a scalar.
- No path traversal into nesting beyond `custom_attributes.<top-level-key>`:
  the whitelist grammar has exactly one segment after the prefix (store
  nested data freely; publish top-level scalars).
- No per-client or per-org mapping overrides; a mapping is global, keyed by
  trigger scope.
- Postgres deployments only — memory mode answers 404 on the self-service
  surface and `DB_CONNECTION_ERROR` on the admin CRUD.

## 8. End-to-end example

1. The user stores the attribute (§2 curl).
2. An admin creates the mapping (§3 curl) — trigger scope `hr`, claim
   `employee_id`, source `custom_attributes.employee_id`, `both` surfaces.
3. The RP requests `scope=openid hr` in the authorization request (the scope
   must exist in the catalog and be allowed for the client).
4. Result — `id_token` payload gains:

```json
{
  "sub": "0192f0a4-…",
  "employee_id": "E-4102",
  …
}
```

   and `GET /oauth2/userinfo` (same token) carries `"employee_id": "E-4102"`.
5. `/.well-known/openid-configuration` now lists `employee_id` in
   `claims_supported` — RPs can discover the extension claim instead of
   hard-coding it.

For the scope catalog itself (creating/allowing the `hr` trigger scope), see
the API reference (`/api/admin/scopes*`); for how scopes gate the standard
claims, see the [OIDC Integration Guide](oidc-guide.md).
