---
sidebar_position: 5
---

# Token Lifecycle

How tokens are born, live, and die in fulla: the three token types, what is
actually stored, how refresh rotation detects theft, and every knob that
controls a lifetime. For the storage schema behind this, see
[Data Persistence](../architecture/data-persistence.md); for the HTTP
contract, see the [API Reference](api-reference.md) and
[ADR-0004](../adr/ADR-0004.md).

## 1. Three token types, three different shapes

| Token | Shape | Validated by | Lifetime default |
|---|---|---|---|
| Access token | **Opaque random string** (`generateSecureToken`) | Server-side state (introspection / userinfo) — never a self-contained JWT | 3600 s (`access_token_ttl`) |
| Refresh token | Opaque random string + **family id** | Server-side state; rotation on every use | 30 days (configurable) |
| id_token | **RS256 JWT** signed via JWKS (`kid` published at `/.well-known/jwks.json`) | Client-side signature verification (standard OIDC) | Same as access token; issued only with the `openid` scope |

Design rationale ([ADR-0004](../adr/ADR-0004.md)): opaque access tokens keep
the authoritative validity decision in server-side state. Redis caches and
replicas have their own convergence windows (§7); stateful revocation does
not imply that every replica rejects the next request. The JWT capability
is reserved for the OIDC `id_token`, where the protocol requires it.

One frequent confusion: the **roles** that accompany a token response are part
of the JSON envelope (`"roles": [...]`) and the id_token claims — the opaque
access token itself carries nothing that needs decoding.

## 2. Issuance paths

All grants converge on `TokenService` (libs/oauth2):

| Grant | Mints | Notes |
|---|---|---|
| `authorization_code` (+ PKCE, mandatory for PUBLIC clients) | access + refresh (+ id_token if `openid`) | The code is single-use with atomic consume (see §5) |
| `refresh_token` | new access + **new** refresh | Old refresh is revoked; family id is inherited (§4) |
| `client_credentials` | access only (M2M, no user) | CONFIDENTIAL clients only; for organization-owned applications the token additionally stores `org_id` (v1.4.0), exposed via introspection when available — the subject is `client:<client_id>` |
| `device_code` | access + refresh after user approval | Polling per RFC 8628 |

`expires_in` in every response advertises the **configured** access-token
lifetime — not a hardcoded 3600 (RFC 6749 §5.1).

## 3. What is stored (and what is not)

- Access and refresh tokens are persisted **only as SHA-256 hashes**
  (`hashToken()` before any repository write) — a database dump exposes no
  usable credentials ([ADR-0004](../adr/ADR-0004.md)).
- The refresh token row additionally stores its `token_family` id, the
  associated access-token hash, `revoked`/`revoked_at`/`revoked_by`.
- Rows past their TTL are deleted by `OAuth2CleanupService`
  (`cleanup_interval_seconds`, default 3600 s) — Postgres via periodic DELETE,
  Redis historically via TTL, memory via periodic sweep.

## 4. Refresh rotation, reuse detection, family revocation

Every successful refresh grants a **new** refresh token that inherits the
same `token_family` (V008). In the production PostgreSQL path, detecting a
retained, previously revoked RT triggers family revocation; attacker and
legitimate user may both need to reauthenticate. Expiry or a missing row
alone is not evidence of replay. The development Memory backend prevents
reuse with an in-process CAS, but its legacy lookup hides revoked records;
it does not provide the same retained-record replay handling as PostgreSQL.
The threat model is described in
[Security Architecture §7](../architecture/security-architecture.md).

There is no refresh grace period or retry-idempotency guarantee. In the
current implementation, old-RT consumption and replacement-pair persistence
are separate repository calls. A persistence failure can consume the old RT;
a lost response can hide an already persisted replacement pair. A network
error does not prove rollback or make the old RT safe to reuse. Stop a failed
refresh loop and reauthenticate; coordinate refreshes across tabs/processes.

**Unreleased hardening:** [PR #321](https://github.com/voidvec/fulla/pull/321)
adds atomic rotation, checked repository results and stronger cache budgets;
its custom-repository migration applies when upgrading to a release that
includes it. These guarantees must not be assumed for earlier builds.

## 5. Single-use of authorization codes

`consumeAuthCode` is atomic per backend: Postgres uses
`UPDATE ... WHERE consumed = false ... RETURNING`, Redis used a Lua script,
memory uses a mutex — so a stolen code replayed in a race loses, always. The
contract is enforced across all three implementations by
`GrantRepositoryContractTest` (`ctest -L Contract`).

## 6. Revocation surfaces

| Surface | Granularity |
|---|---|
| `POST /oauth2/revoke` (RFC 7009) | The presented token |
| Admin API `DELETE /api/admin/tokens/{prefix}` | By token prefix |
| Admin token management | Revoke by **client** or by **user** — wipes every token minted for that principal |

After a successful authoritative revocation, a read of that state rejects
the token. An existing cache snapshot or stale replica can delay observation
(§7). Always check introspection's `active` value first; ignore any metadata
when it is false. Userinfo rejects a token it observes as revoked with 401.

Login eligibility, current authorization and token validity are different:

| Administrative change | Existing behavior and boundary |
|---|---|
| Disable a user | Sets the login lock and invalidates user read caches; does not automatically revoke every issued AT/RT. Explicit token revocation is a separate action. |
| Remove a user's role assignment | Role-protected APIs resolve role/permission data. Token scopes are not rewritten; role-cache invalidation and its lifetime still matter. |
| Change a role's permissions | Uses a separate process-local permission cache without a TTL; invalidate it on every replica or restart. The user-role TTL does not bound these changes. |
| Remove organization membership or consent | Organization gates and claim resolution check their own current state. The stored token `org_id` is historical context, not a live membership grant; issued tokens are not automatically revoked. |
| Suspend an open-platform client | Client validation and introspection's owner-status overlay have separate caches. This operation is not universal token revocation for every API. |

Organization-decision atomicity and response-completion hardening are tracked
in [PR #322](https://github.com/voidvec/fulla/pull/322); they do not change these
issued-token revocation boundaries.

## 7. Cache interplay

With Redis L2 enabled (`cache.enabled`), token/client reads check
`fulla:cache:*` before PostgreSQL. Direct AT revocation attempts a negative
marker and immediate/delayed deletion of access and introspection entries.
The second DEL defaults to 200 ms (`cache.invalidation_double_delete_delay_ms`,
clamped to 50–2000 ms). This reduces refill races; it does not prove that
every invalidation succeeds. Family/RT revocation and pair writes pass
through to storage and do not all perform positive-cache invalidation.

| Cache | Default entry lifetime |
|---|---|
| Access/introspection | `cache.ttl_seconds.access_token_max`: 60 s, capped by token expiry |
| Client | `cache.ttl_seconds.client`: 300 s |
| User roles / profile | `cache.ttl_seconds.user_roles`: 120 s / `user_profile`: 300 s |
| Owner status for introspection | 60 s process-local memo, separate from the client cache |

These are entry lifetimes, not a proven deadline measured from revocation.
Current token refill computes a relative TTL after the backing read;
in-flight reads, queued writes and replica lag can extend observation beyond
one configured TTL after a change. A delayed second DEL is also best effort.
Do not infer an absolute cross-replica revocation SLO from these defaults.
The absolute token-cache budgets proposed in PR #321 must not be generalized
to the other caches.

Current Redis token payloads do not preserve `org_id`, so a cache hit may
omit it. Absence is not proof of personal context; resource servers must
establish required organization authorization separately. Invalidation
failures are observable through `fulla_cache_invalidation_failures_total`
([Observability](../operate/observability.md)). See
[Data Persistence](../architecture/data-persistence.md) for the storage paths.

## 8. Configuration reference

| Key | Default | Meaning |
|---|---|---|
| `access_token_ttl` | 3600 | Access-token lifetime (seconds); advertised in `expires_in` |
| refresh-token TTL | 30 days | Refresh-token lifetime |
| `cleanup_interval_seconds` | 3600 | How often `OAuth2CleanupService` purges expired rows |
| `cache.enabled` / `cache.ttl_seconds` | false / — | L2 cache for token/client reads ([Configuration Guide](../operate/configuration-guide.md)) |
