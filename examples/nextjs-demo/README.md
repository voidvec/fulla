# fulla × Next.js demo (App Router)

P1-5 #282 — a third-party web app talking to fulla **only** through the
[`fulla-oauth2` SDK](../../clients/ts) (used here as a `file:` dependency —
exactly the surface npm consumers get). The flow is the full browser dance:
login → callback (state + PKCE verified) → session → userinfo → protected
API → RP-initiated logout. Tokens live exclusively in httpOnly cookies.

Clone-to-login target: **≤ 30 minutes** (README-timed; steps below, ~10 min
on a dev laptop with Docker).

## The 30-minute path

Prereqs: Docker, Node ≥ 18.17 (20+ recommended), and a C++ toolchain only if
you build the server from source instead of using compose.

### 1. Start fulla (5 min)

```bash
git clone https://github.com/voidvec/fulla.git && cd fulla
docker compose -f deploy/docker/docker-compose.yml up -d --build
# dev seed credentials: admin / admin (see README "Quick Start")
curl http://localhost:5555/.well-known/openid-configuration | head -c 300
```

The compose stack applies migrations + seeds, including the
`fulla-portal` PUBLIC client this demo logs in with. Its registered
redirect family already contains `http://localhost:8080/callback` — which
is why this demo pins port **8080**. (No secret exists: PUBLIC client +
PKCE S256.)

### 2. Build the SDK once (2 min)

The demo depends on the package's compiled output (`dist/`), like a real
npm consumer:

```bash
cd clients/ts && npm install && npm run build
```

### 3. Run the demo (2 min)

```bash
cd ../../examples/nextjs-demo
cp .env.example .env.local
npm install
npm run dev        # next dev -p 8080
```

Open http://localhost:8080 → **Sign in with Fulla** → log in as
`admin` / `admin` → you land on `/profile` with your userinfo claims.
"protected API" link shows what `/api/me` (a Bearer-authenticated route)
sees; "End session" performs RP-initiated logout.

## What each piece demonstrates

| Route | SDK call / role |
|---|---|
| `GET /api/login` | `PkcePair.generate()` + `flow.buildAuthorizeUrl({ state, nonce, pkce })` |
| `GET /api/callback` | `flow.completeAuthorizationFlow(url, { state, codeVerifier })` — state mismatch / error callbacks throw BEFORE any exchange |
| `/profile` | `userinfo` with the session's Bearer (scope-gated claims) |
| `GET /api/refresh` | `flow.refresh(old)` — fulla rotates; the response's new refresh token replaces the old |
| `GET /api/me` | protected API: server-side Bearer call from an httpOnly cookie |
| `GET /api/logout` | `flow.buildEndSessionUrl({ idTokenHint, postLogoutRedirectUri })` |

## e2e smoke (Playwright)

```bash
npx playwright install chromium   # once
npm run test:e2e                  # expects fulla on :5555 and the demo on :8080
```

The same spec is the **fourth interop driver** in `.github/workflows/
interop.yml`: unlike the three json-mode RP libraries, it drives a real
browser through this app.

## Notes

- `.env.example` carries only PUBLIC client values (no secrets — Secret
  Hygiene gate compliant).
- The session cookie is httpOnly/SameSite=Lax and lives one day; the
  transaction cookie (state + PKCE verifier) ten minutes.
- This demo intentionally avoids NextAuth/middleware: the point is to show
  the SDK doing the protocol work.
