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
the server in full control — revocation is immediate and stateful, and no
key-compromised token outlives its server-side record. The JWT capability is
reserved for the OIDC `id_token`, where the protocol requires it.

One frequent confusion: the **roles** that accompany a token response are part
of the JSON envelope (`"roles": [...]`) and the id_token claims — the opaque
access token itself carries nothing that needs decoding.

## 2. Issuance paths

All grants converge on `TokenService` (libs/oauth2):

| Grant | Mints | Notes |
|---|---|---|
| `authorization_code` (+ PKCE, mandatory for PUBLIC clients) | access + refresh (+ id_token if `openid`) | The code is single-use with atomic consume (see §5) |
| `refresh_token` | new access + **new** refresh | Old refresh is revoked; family id is inherited (§4) |
| `client_credentials` | access only (M2M, no user) | CONFIDENTIAL clients only; for organization-owned applications the token additionally carries the `org_id` claim (v1.5.0), exposed via introspection — the subject stays the client id |
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

Every refresh grants a **new** refresh token that inherits the same
`token_family` (V008). If a refresh token that was already revoked is
presented again, that is treated as theft: the server **cascades revocation
to every token in the family** — attacker and legitimate user alike must
re-authenticate. The full threat-model walkthrough (with a sequence diagram)
lives in [Security Architecture §7](../architecture/security-architecture.md).

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

Revocation is stateful and immediate: the very next introspection or
userinfo call with a revoked token returns `{"active": false}` / 401.

## 7. Cache interplay

With the Redis L2 cache enabled (`cache.enabled`), token and client reads hit
`fulla:cache:*` before Postgres. Every write path (issue, refresh, revoke)
invalidates through **delayed double-delete** (immediate DEL + a second DEL
~200 ms later, `cache.invalidation_double_delete_delay_ms`), closing the
race where a concurrent reader re-fills a stale entry between the write and
the first delete. Failures of the second delete are counted in
`fulla_cache_invalidation_failures_total` — see
[Observability](../operate/observability.md). Details in
[Data Persistence · cache consistency](../architecture/data-persistence.md).

## 8. Configuration reference

| Key | Default | Meaning |
|---|---|---|
| `access_token_ttl` | 3600 | Access-token lifetime (seconds); advertised in `expires_in` |
| refresh-token TTL | 30 days | Refresh-token lifetime |
| `cleanup_interval_seconds` | 3600 | How often `OAuth2CleanupService` purges expired rows |
| `cache.enabled` / `cache.ttl_seconds` | false / — | L2 cache for token/client reads ([Configuration Guide](../operate/configuration-guide.md)) |
