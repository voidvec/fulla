# RP Interop Smoke (P0-9)

Three standard relying-party libraries drive fulla's authorization-code +
PKCE (S256) → token → userinfo → refresh → end-session chain in strict mode.
This is the v1.5.0 P0 exit gate ("three libraries, zero warnings") and the
regression harness for later protocol work (PAR, resource indicators).

## Drivers

| Dir | Library | Strict checks it owns |
|---|---|---|
| `node/` | `openid-client` (v6) | discovery, id_token signature/iss/aud/exp/nonce via JWKS, PKCE, state, refresh grant |
| `python/` | `authlib` | authorize-URL + token endpoint interop, refresh grant, userinfo scope gating |
| `go/` | `coreos/go-oidc/v3` + `x/oauth2` | provider discovery, `IDToken.Verify` (JWKS/iss/aud/exp + custom nonce claim), `UserInfo` sub consistency, oauth2 refresh |

All three drive the SAME browser-less recipe: build the authorization URL
with their own state/nonce/PKCE, complete first-factor login via fulla's
json-mode `POST /oauth2/login?json=true` (form-encoded, same params the
authorize redirect flattens), then hand the code back to the library for a
strict token exchange.

## Running

The stack must be up and seeded (dev seeds: `fulla-portal` PUBLIC client
with `openid profile email` + loopback redirects, `admin`/`admin`):

```bash
# e.g. from a build tree (uses the tests config, postgres on 127.0.0.1:5432):
(cd build/windows-msvc/tests/Release && ../../apps/server/Release/fulla-server.exe &)

node tests/interop/node/rp-smoke.mjs
python tests/interop/python/rp_smoke.py
go run ./tests/interop/go
```

Environment (all optional):

| Var | Default |
|---|---|
| `FULLA_BASE_URL` | `http://localhost:5555` |
| `FULLA_RP_CLIENT_ID` | `fulla-portal` |
| `FULLA_RP_REDIRECT_URI` | `http://localhost:5173/callback` |
| `FULLA_RP_SCOPE` | `openid profile email` |
| `FULLA_RP_USER` / `FULLA_RP_PASSWORD` | `admin` / `admin` |

Exit code 0 = all stages green. Any library warning/fallback is a failure —
that is the point of the gate.

## CI

`.github/workflows/interop.yml` runs all three on `master` / `release/**`
pushes and on demand — the v1.5.0 release gate; deliberately not on the
per-PR hot path (runtime).
