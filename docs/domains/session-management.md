---
sidebar_position: 6
---

# Session Management

fulla has **two different lifetimes that people often conflate**: the
browser SSO session (a server-side session behind a cookie) and API tokens
(no session involved at all). This page explains both, how they end, and —
the operationally important part — what the Drogon session layer costs under
machine traffic and how to size it.

## 1. Two lifetimes, one system

| | SSO session | API tokens |
|---|---|---|
| Who has one | Browsers walking the interactive login/consent flow | Any client calling token/introspect/userinfo — **no cookie is ever sent** |
| Backed by | Drogon server-side session (`session_timeout`) | Opaque tokens, hashed at rest ([Token Lifecycle](token-lifecycle.md)) |
| Ends via | `end_session` / logout / idle expiry | Expiry, revocation, refresh-family cascade |

Machine traffic never touches the session store. But — the critical detail
below — with sessions enabled it still *creates* session entries.

## 2. The Drogon session behavior you must know (upstream #278)

With `enable_session: true`, Drogon's framework layer creates a Session for
**every request that arrives without a session cookie** and retains it in the
SessionManager until `session_timeout` expires
([drogon#278](https://github.com/an-tao/drogon/issues/278), verified on this
codebase 2026-08-22). API clients (token / introspect / userinfo / discovery)
never send cookies, so they pay this cost on **every request**.

Measured on a production LTO build (three 60 s c=128 storms):

| Quantity | Value |
|---|---|
| Retained per request | **~750 B** (744/755/759 B across runs) |
| Steady-state formula | `API_QPS × session_timeout × 750 B` |
| Discovery throughput tax | **~-54%** (164.6k → 76.3k QPS, session OFF/ON interleaved 6×) |

The tax affects every endpoint (session creation happens in the framework
layer, before routing). Historical benchmark numbers (S1 87–104k) were
measured **with** sessions on; the session-less ceiling is ~165k.

### Sizing table

Sizing by the formula (interactive logins write-then-read within
milliseconds, so correctness never depends on the TTL — verified across the
full S4 login/authcode ladder at 120 s):

| API QPS (cookie-less) | TTL 3600 (default) | TTL 300 | TTL 120 | TTL 30 |
|---|---|---|---|---|
| 100 | ~0.3 GB | ~23 MB | ~9 MB | ~2 MB |
| 1,000 | **~2.7 GB** | ~225 MB | ~90 MB | ~23 MB |
| 10,000 | **~27 GB (OOM zone)** | ~2.2 GB | ~0.9 GB | ~225 MB |

**Guidance**:

- Mostly-interactive deployments (under 100 QPS of API traffic): keep the default 3600 s —
  the full SSO experience survives.
- Non-trivial API traffic: lower `session_timeout` (and `session_max_age`
  together) to a row your memory budget tolerates. A 2-minute idle window is
  acceptable for browser SSO (OIDC deployments commonly use 5–15 min).
- The benchmark profile uses 30 s (`config.bench.json`,
  `QPS × 30 × 750 B` capped).
- The throughput tax (not the retention) is independent of TTL and exists
  whenever sessions are enabled; the structural fix is upstream lazy /
  per-path session creation — tracked in
  [drogon#278](https://github.com/an-tao/drogon/issues/278).

Deployment-time operational summary: [Production Deployment · performance
tuning](../operate/deployment.md).

## 3. Ending a session

### RP-Initiated Logout (F-027) — `GET/POST /oauth2/end_session`

Terminates the server-side session and (optionally) redirects. Rules:

- `post_logout_redirect_uri` **must** be one of the client's registered
  redirect URIs; the client is identified by the `id_token_hint`'s `aud`.
- The hint's **signature is verified** (RS256 + kid + iss/exp/sub policy).
  A failed verification → 400 `AUTH_INVALID_ID_TOKEN_HINT` (4006).
- No valid hint + registered URI → 400. On success: 302 with `state`
  echoed, or 200 when no redirect URI was supplied.

### API logout — `POST /oauth2/logout`

Revokes the presented tokens **and** calls `session()->clear()` (F-028), so
the server-side session dies together with the access token.

### Re-authentication semantics (F-022)

`prompt=login` forces re-authentication even with a live session;
`prompt=none` forbids UI (errors `login_required` / `consent_required` are
redirected back to the verified redirect URI); `max_age=<seconds>` forces
re-auth when the session's `auth_time` is older. `auth_time`, `amr`, and
`acr` (1 = password, 2 = MFA) travel on the authorization code and are
stamped into the id_token — see
[Configuration Guide §6](../operate/configuration-guide.md).

### What a relying-party login does once the SSO session has ended

Token refreshes do not extend the SSO session. An access/refresh token pair
stays valid for its own lifetime, but once the SSO session has ended (idle
expiry or `session_max_age`), `/oauth2/authorize` treats the user as signed
out again and sends them to the login page with the authorize request
preserved on the URL — the same thing happens on `prompt=login` or when
`max_age` is exceeded. The user signs in once more (a fresh SSO session with
a fresh `auth_time` and `amr`) and the authorization resumes, so the relying
party still receives its response. This is why a user who remains signed in
to the portal is asked to sign in again when an application they come back to
the next day starts a login.

## 4. What sessions are NOT

- Token revocation surfaces (revoke by token / client / user) act on
  **tokens**, not SSO sessions — see [Token Lifecycle §6](token-lifecycle.md).
- The admin console's token browser lists at-rest tokens; it does not expose
  live session inventory.
