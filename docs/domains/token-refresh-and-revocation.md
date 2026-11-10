# Refresh Tokens and Revocation Boundaries

The S03 security hardening keeps single-use refresh tokens and family reuse detection. It changes how rotation is persisted and makes cache convergence explicit. This page describes the unreleased behavior and the migration required for custom C++ repositories.

## 1. Rotation and Failure Outcomes

The token endpoint authenticates the client before refreshing. The repository then checks that the RT belongs to that client, is unexpired and is unused. A wrong client cannot consume an unused RT or revoke another client's family by presenting a used RT.

Subject normalization, claim resolution and random token generation happen before consumption. Memory rotation holds one mutex; PostgreSQL rotation holds a transaction-scoped family advisory lock, consumes the old RT and inserts both new tokens in one transaction. Success is reported only after a confirmed commit. Rotation and family revocation use the same lock domain.

| Failure or event | Persisted state | Client outcome |
|---|---|---|
| Lookup/storage error before consumption | Old RT unchanged; no new pair | `server_error`; no tokens |
| Random generation or subject normalization failure | Old RT unchanged; no new pair | Error; no tokens |
| Client mismatch or an unused RT with `now >= expires_at` | No consumption or family revocation | `invalid_grant` |
| Access insert, refresh insert, or confirmed commit rejection | Old consumption rolls back; no new pair | `server_error`; no tokens |
| COMMIT confirmation lost | Complete old state or complete rotated state; outcome unknown to caller | `server_error`; no unconfirmed tokens |
| Commit succeeds but HTTP response is lost | Old RT used; complete new pair exists | Reauthentication may be required |
| Correct client's retained used RT is replayed | Entire family, including committed rotated pairs, is revoked | `invalid_grant`; revocation storage failure is `server_error` |

An unknown commit outcome is different from a definite rollback. Never infer that an RT is reusable from a network error. Retrying a used RT can revoke the family, including the pair whose response was lost. There is no grace period or server-side retry idempotency. Coalescing refreshes within one client instance does not coordinate other tabs or processes.

Two requests using one old RT can produce at most one successful rotation. A successful response does not promise that its pair remains active if another request subsequently detects reuse. Expiry alone is not proof of reuse. A retained used record remains reuse evidence even after expiry, preserving the existing Postgres policy; it can never be exchanged again.

Cleanup retains expired RT metadata while its associated AT remains active, including configurations whose RT lifetime is shorter than the AT lifetime. This preserves family revocation reachability without extending RT validity. After that AT expires, is revoked or disappears, a later sweep can reclaim the metadata. No permanent tombstone or schema change is introduced.

## 2. C++ Repository Migration

Custom `ITokenRepository` implementations must implement these additive methods:

- `inspectRefreshToken`: authoritative lookup with `Ready`, `NotFound`, `AlreadyUsed`, `ClientMismatch`, `Expired` and `StorageError`; only a correctly bound client receives used-token metadata.
- `rotateRefreshToken`: revalidate the authoritative old record and expiry inside the critical section, atomically consume and save the pair, and return `Rotated` only after confirmed persistence.
- `revokeTokenFamilyChecked`: atomically revoke both token kinds under the rotation family lock and report confirmed success/failure.

The original CRUD/CAS methods remain available. The new methods have fail-closed defaults, so an old implementation still compiles, but its refresh requests return `server_error` until migrated. This is source compatibility, not behavioral compatibility. There is no binary ABI guarantee. Do not implement the new result by treating every old `nullopt` as reuse, or by claiming atomicity for sequential writes.

Drogon hosts must use `lookupOwnerStatusChecked` for introspection ownership decisions. `OwnerStatusResult` separates Active, confirmed Unmanaged, Suspended (including unknown row statuses) and Unavailable. Only the first two permit introspection. Deployments without an owner registry (`storage_type` other than postgres) classify as Unmanaged — a structural fact known at startup, consistent with memory mode; Unavailable is reserved for registry query errors and timeouts, so a mode without a registry never reports valid tokens as inactive. The old optional `lookupOwnerStatus` wrapper is retained for display/source compatibility and loses the error/absence distinction; it must not authorize access.

The supplied Memory/Postgres repositories and Redis cache decorator implement the new path. The deprecated standalone Redis storage still rejects the refresh grant. Publish this security behavior change in the next MINOR release with the migration notice, following the [versioning policy](../contribute/versioning-and-release.md).

## 3. Token Cache Convergence

PostgreSQL access-token validation and introspection read the master, including cache fallback. A lagging reader is not an authoritative source for security cache refill. This increases master read traffic on misses and failures; size the primary accordingly.

The token L2 cache uses the configured `cache.ttl_seconds.access_token_max` (default 60 seconds), capped by token expiry. The expiration budget starts before the asynchronous lookup. Redis `EXAT` retains that absolute deadline even if a database callback or Redis command arrives late. A Lua operation checks the revocation marker and deadline atomically with refill; introspection also verifies the access-key discriminator, so RT introspection is never cached. Access/introspection payloads preserve `org_id`; older unversioned payloads are treated as misses.

Direct access-token revocation attempts a negative marker and immediate/delayed deletion. Family revocation bypasses positive-cache invalidation and converges through the existing TTL policy. If invalidation fails, an already cached pre-revocation token can remain usable until the earlier of its absolute cache deadline and token expiry. Under synchronized application/Redis clocks and authoritative master reads, a pre-revocation snapshot cannot extend that deadline beyond one configured cache budget after revocation. Account for clock skew operationally; a TTL is not a promise that every replica rejects the next request.

Cached read requests have one five-second local completion deadline spanning Redis lookup, the backing read and guarded refill. Initial Redis errors fall back to the authoritative repository. A guarded-refill error or local deadline rejects the read; it does not return an unconfirmed stale snapshot. Late callbacks cannot complete twice, and the business callback is released. Hosts must run the Drogon event loop used for these timers. Timeout/error counters use `fulla_cache_total{repo="token",outcome="error"}`; failed DEL attempts retain `fulla_cache_invalidation_failures_total{kind="token"}`. A failed negative-marker SET is observable too.

## 4. Permission Changes Are Separate Operations

Token validity, login eligibility and current authorization are distinct checks. Existing policies are not changed into universal immediate revocation:

| Change | API and refresh behavior | Userinfo/introspection and cache boundary |
|---|---|---|
| Disable a user | Sets the login lock; it does not automatically revoke all issued ATs/RTs | Do not treat profile-cache invalidation as token revocation. Explicitly revoke affected tokens when terminating existing access |
| Revoke a user's role | Role-protected APIs resolve current role/permission data; existing token scopes are not rewritten | User-role cache defaults to 120 seconds; write hooks invalidate known subject forms. Userinfo claims are not a replacement for an API authorization check |
| Remove organization membership | Organization-aware gates and claim resolution check membership; the stored `org_id` remains a historical token binding | New userinfo/refresh ID-token `org_ctx` is omitted when membership is absent. Introspection `org_id` does not prove current membership |
| Suspend an open-platform client | New client lookup rejects a positively suspended owner; an existing AT is not universally revoked by that lookup | Introspection has a typed owner-status overlay: query errors/timeouts produce only `active:false`, never an active result. Client and owner-status caches remain separate; other APIs do not acquire this overlay |

Owner-status memoization has a 60-second read-start budget and a five-second completion deadline. Only successful Found/NoRow results are cached; errors/timeouts and their late callbacks cannot refill the memo. Invalidation can still race an in-flight successful read within its original budget. Other existing caches retain their own policies: client L2 defaults to 300 seconds, user roles to 120 seconds and user profiles to 300 seconds. These are entry lifetimes, not proven commit-to-rejection deadlines: their asynchronous refill and backing read duration must also be considered. The token-cache absolute-budget guarantee above must not be generalized to these caches. A backing read or replica without a bounded freshness/latency guarantee cannot establish a stricter global revocation SLO merely by configuring a TTL.

The role-removal row refers to removing a user's role assignment. Editing the permissions granted by an existing role uses a separate process-local `RolePermissionResolver` cache with no TTL. Its current policy requires invalidation on every replica (or restart); the user-role TTL does not bound permission-definition changes. This work does not add a cross-process invalidation protocol.

If immediate session termination is required, perform and verify explicit token revocation. Without that operation, disabling login or changing roles must not be advertised as revoking existing tokens. A request already being authorized when a change commits is also different from a new request starting afterwards.

## 5. Validation and Operational Limits

The regression suite covers Memory/Postgres binding, collisions at either pair insert, deferred commit rejection, single-use concurrency, family-lock expiry and a controlled CAS-to-INSERT pause during family revocation. Domain tests cover lookup/rotation errors, random-source failure, unknown commit outcomes, response-loss/replay semantics and old-only SDK implementations. Owner-status tests separately cover policy mapping, missing backends and real PostgreSQL lock timeouts without confusing a preceding client-authentication error with overlay behavior. Cache tests use independent node objects sharing real Redis, plus command faults and deferred snapshots; this models shared L2 behavior and does not claim a multi-host deployment test.

Keep failure records distinct: deterministic database rejection, unknown database commit outcome and lost HTTP response have different evidence. Do not claim a simulated acknowledgement-loss test is a real network-partition experiment. No token format, default token lifetime, consent policy, organization permission rule or production deployment is changed by this work.
