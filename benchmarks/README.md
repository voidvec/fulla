# Fulla HTTP performance benchmarks

This directory holds the **reproducible, out-of-process** HTTP benchmarks for
Fulla — the Phase 0 "credibility baseline" of the
[productization evolution plan](../docs/productization-evolution/productization-evolution-plan.md).
Its purpose is to turn the load-bearing performance claims in the
[research report §3.1](../docs/productization-evolution/productization-research.md)
(QPS / latency / memory / cold-start) from **assertions into measurements**.

> Full design (scenario matrix, methodology, acceptance criteria): see
> [benchmark-facility-design.md](../docs/productization-evolution/in-progress/benchmark-facility-design.md).
> This README is the **how-to-run** companion; the design doc is the **why**.

## Scope (M1–M3)

| | |
|---|---|
| **Target** | postgres + redis full stack via `deploy/docker/docker-compose.yml` (backend on `:5555`), **not** memory mode |
| **Scenarios (M1)** | **S1 discovery** + **S2 client_credentials** — stateless / single-step, validate the harness |
| **Scenarios (M2)** | **S3 introspect** (active-token RS256 verify) + **S4 auth_code+PKCE** (multi-step login→token, heaviest path) |
| **Scenarios (M3)** | **S5 refresh_token** (rotation, one-shot RT pool) + **S6 userinfo** (bearer filter + user lookup) |
| **M3 additions** | Resource observation (`--observe`: docker-stats + /metrics scraping), cold-start measurement |
| **Phase 0.5 (delivered)** | Same-environment competitor comparison (Keycloak / Ory Hydra / Zitadel) — see [competitors/README.md](competitors/README.md) and [results/COMPARISON.md](competitors/results/COMPARISON.md) |

### Scenario summary

| # | Scenario | Endpoint | Key constraint |
|---|----------|----------|----------------|
| S1 | discovery | `GET /.well-known/{openid-configuration,jwks.json}` | Stateless — Drogon framework ceiling |
| S2 | client_credentials | `POST /oauth2/token` | HTTP Basic auth (F-017), single-step |
| S3 | introspect | `POST /oauth2/introspect` | Must use **active** token (malformed = fast path, inflates QPS) |
| S4 | auth_code+PKCE | `POST /oauth2/login` → `POST /oauth2/token` | PKCE mandatory (F-011), multi-step, per-VU user rotation |
| S5 | refresh_token | `POST /oauth2/token` | Each RT used once (V008 family rotation), `--reseed` per level |
| S6 | userinfo | `GET /oauth2/userinfo` | Bearer user AT (not client_credentials AT) |

## Prerequisites

- **Docker** + **docker compose** (to bring up the target stack)
- **wrk** (the load driver; C, event-driven, low client overhead — TechEmpower
  same):
  - Debian/Ubuntu: `sudo apt-get install -y wrk`
  - macOS: `brew install wrk`
  - Alpine: `apk add --no-cache wrk`
- **python3** (for the result parser; no extra packages needed)
- **curl** (for health/seed checks in `setup.sh`)

No local PostgreSQL/Redis is required — both run inside the compose stack.

## Quick start

```bash
# 1. Boot the full stack, gate on /health/ready, validate seed data,
#    generate token pools (S3/S5/S6) + PKCE pairs (S4), warm up bench users.
bash benchmarks/fulla/setup.sh

# 2. Run M1 scenarios (single-step, no dependencies):
bash benchmarks/fulla/run-scenario.sh scenarios/s1-discovery.lua
bash benchmarks/fulla/run-scenario.sh scenarios/s2-client-credentials.lua

# 3. Run M2 scenarios (token pools required — setup.sh generates them):
bash benchmarks/fulla/run-scenario.sh scenarios/s3-introspect.lua
bash benchmarks/fulla/run-scenario.sh scenarios/s4-auth-code.lua

# 4. Run M3 scenarios:
#    S5 refresh_token: each RT consumed once → --reseed refreshes the pool per level
bash benchmarks/fulla/run-scenario.sh scenarios/s5-refresh-token.lua \
    --reseed benchmarks/fulla/lib/generated/bench_refresh_tokens.sql
#    S6 userinfo: token pool reusable
bash benchmarks/fulla/run-scenario.sh scenarios/s6-userinfo.lua

# 5. (optional) Run with resource observation (docker-stats + /metrics scraping):
bash benchmarks/fulla/run-scenario.sh scenarios/s2-client-credentials.lua --observe

# 6. (optional) Quick smoke check with just a few low levels:
bash benchmarks/fulla/run-scenario.sh scenarios/s2-client-credentials.lua 2 4 8

# 7. (optional) Measure cold-start time + RSS peak:
bash benchmarks/fulla/measure-cold-start.sh
bash benchmarks/fulla/measure-cold-start.sh --pre-migrated   # exclude migration time

# 8. Tear down + reset volumes (deterministic for the next run).
bash benchmarks/fulla/teardown.sh
```

## Competitor comparison (Phase 0.5)

`benchmarks/competitors/` extends this facility to same-environment
competitor benchmarks (Keycloak / Ory Hydra / Zitadel) — same machine, same
wrk staircase, same PostgreSQL backend, official recommended configs. Full
methodology: [competitor-benchmark-design.md](../docs/productization-evolution/in-progress/competitor-benchmark-design.md).

```bash
# One command, four products, serial with full teardown between products:
bash benchmarks/competitors/run-comparison.sh --fresh

# Single product (resume / re-run):
bash benchmarks/competitors/run-comparison.sh --only keycloak   # or ory|zitadel|fulla

# Regenerate the report from committed JSONs only:
bash benchmarks/competitors/run-comparison.sh --report-only
```

Requirements on top of the above: `python3 -m pip install requests pyjwt cryptography`
(token-pool minting for competitor stacks). Results land in
`benchmarks/competitors/results/` (schema v1 + `product`/`product_version` env
fields); the aggregated report is `benchmarks/competitors/results/COMPARISON.md`.
Token pools and runtime credentials are minted at setup time and gitignored —
nothing secret is committed.

Each level writes one JSON result to `benchmarks/results/` named
`<YYYYMMDD>-<git-sha>-<scenario>-c<conn>.json` (see
[`fulla/lib/result-schema.md`](fulla/lib/result-schema.md) for the
schema). The run prints a one-line summary per level, e.g.:

```
=== s2-client-credentials  c=64  t=4 ===
  -> 20260808-abc1234-s2-client-credentials-c64.json: QPS=50,780  P99=0.003s  err=0.0008%  driver_cpu=42.5%
```

## Reading the results

- **QPS** (`qps`): primary throughput. Find the **knee** — the level beyond
  which QPS stops growing (~5% over two consecutive levels) while P99 climbs.
- **P99** (`latency_us.p99`): tail latency. The research report claims <2ms;
  M4 will verify this honestly.
- **error_rate**: must be `< 0.01%` for a level to count as the **steady-state
  capacity** (what gets reported externally), per AC5.
- **driver.limited**: `true` when the wrk process hit ≥80% CPU — the number is
  then a **lower bound**, not a ceiling (AC4). If this fires, use a stronger
  driver machine or multiple wrk instances.

## Configuration knobs (env vars)

| Variable | Default | Effect |
|---|---|---|
| `TARGET_URL` | `http://127.0.0.1:5555` | where the backend is reachable |
| `WARMUP_S` | `10` | per-level warmup seconds (discarded) |
| `DURATION_S` | `30` | per-level measured-run seconds |
| `DRIVER_CPU_GATE` | `80` | wrk CPU% at/above which a level is marked `limited` |
| `KEEP_VOLUME` | `0` | `=1` makes setup/teardown preserve the DB volume |
| `SKIP_TOKEN_GEN` | `0` | `=1` skips token pool generation in setup.sh (if already generated) |
| `SKIP_WARMUP` | `0` | `=1` skips the PBKDF2 rehash warmup (if bench users already warmed) |
| `BENCH_TARGET_SPEC` / `BENCH_DRIVER_SPEC` | empty | recorded into each result's `env` block (e.g. `"4vCPU/8GB"`) |

### Benchmark config (`config.bench.json`)

`setup.sh` automatically swaps `apps/server/config/config.bench.json` over
`config.json` before starting the stack (PG=64 connections, Redis=64,
cache enabled, log_level=WARN; see the bench overlay in
`benchmarks/fulla/docker-compose.bench.yml` for the PG instance tuning).
The original `config.json` is backed up to
`config.json.dev-backup` and restored by `teardown.sh`. This avoids touching
the dev/prod configs while giving the benchmark larger connection pools. The
swap is detected by file existence — no flags needed.

## How the seed data works

The docker-compose stack does **not** auto-seed the database. Schema migrations
run on the app's startup (`FULLA_AUTO_MIGRATE=true` → `MigrationRunner`), but
the app never applies seed SQL, and the postgres entrypoint's `initdb.d/seed/`
subdirectory mount is a documented no-op (postgres does not recurse into
subdirs). So `setup.sh` **explicitly applies** every `apps/server/seed/*.sql`
file via `docker exec ... psql` after the stack boots, retrying until the
migration-created tables exist (the migration runs on a detached startup thread).

The seed files are:

- `dev_backend_client.sql` — the `backend-svc` CONFIDENTIAL client (S2 needs this)
- `dev_vue_client.sql` — the `fulla-portal` PUBLIC client (M2 S4/S5/S6)
- `dev_admin_user.sql` — `admin/admin` (**smoke only; do not load-test** —
  progressive lockout at 5/10/15/20 failed logins, `AuthService.cc:149-157`)
- `bench_users.sql` — **512 dedicated `bench_user_NNNN` users** (password
  `admin`, same legacy SHA256+salt). M1 scenarios do not consume these; they
  are seeded now so M2's auth_code scenario can bind each virtual user to a
  distinct account (avoiding lockout). Their first login triggers a PBKDF2
  rehash; `setup.sh` exposes a `warmup_bench_users` function M2 will invoke
  before timed runs so that CPU cost lands in warmup, not measured throughput.

All seed files are idempotent (`ON CONFLICT DO NOTHING`), so re-applying on an
existing volume is safe.

### M2–M3 token pool generation

`setup.sh` also generates benchmark token pools (via `lib/gen-tokens.py`) for
scenarios that need pre-seeded tokens:

- **Access tokens** (2000) — for S3 (introspect) and S6 (userinfo). These are
  **reusable**: introspect/userinfo validate but don't consume the token.
  Seeded with `client_id=backend-svc`, `scope=openid profile`, far-future
  `expires_at`, user-scoped subjects (`bench_user_NNNN`).
- **Refresh tokens** (20000) — for S5 (refresh_token). Each RT is **consumed
  once** (V008 family rotation). Seeded with `client_id=fulla-portal` (has
  `refresh_token` grant), unique `family_id` per token. Use `--reseed` in
  `run-scenario.sh` to refresh the pool before each concurrency level.
- **PKCE pairs** (512) — for S4 (auth_code). wrk's Lua has no SHA256, so we
  pre-generate S256 verifier/challenge pairs. Each pair = one login attempt.

Token hashing matches `TokenCrypto::hashToken` (`TokenCrypto.cc:26-37`):
`toUpperCase(sha256Hex(rawToken))`. The `token` column stores the hash; the
Lua scripts send the **raw** token in requests.

Generated files go to `lib/generated/` (gitignored — they contain raw token
secrets, regenerated by `setup.sh`). Skip with `SKIP_TOKEN_GEN=1`.

**Determinism**: `teardown.sh` removes volumes by default (`docker compose
down -v`), so every `setup.sh` starts from the same schema + seed. Use
`KEEP_VOLUME=1` to re-run against an existing stack.

## Known limitations

- **CI shared runners give non-credible absolute numbers** — neighbors steal
  CPU, so absolute QPS/latency from a CI run are meaningless. CI regression
  gates (a future `benchmarks-regression.yml`, design §9) will only compare
  **relative** deltas against a master baseline. For externally-cited absolute
  numbers, use a **dedicated benchmark machine** and record its spec in the
  result `env`.
- **Driver must not be the bottleneck.** A single wrk instance saturating its
  CPU cannot drive a fast C++ server to its limit. If `driver.limited` fires,
  the reported QPS is a lower bound — switch to a beefier driver or aggregate
  multiple wrk instances.
- **S1–S6 in M1–M3.** The full scenario matrix is implemented. The
  load-bearing-assumption verification report (M4) and competitor comparison
  (Phase 0.5) are not yet done.
- **S4 login rate-limiting.** The token endpoint has a per-(IP, client_id)
  rate limit (30 failures / 60s, `config.json:184-187`). All VUs share the
  same IP + `fulla-portal` client_id, so failures accumulate on one bucket. With
  correct per-user rotation, failures should be ~0 — but if error_rate spikes
  at high concurrency, this is the likely cause (a real product characteristic,
  not a benchmark bug).
- **S5 RT pool exhaustion.** Each refresh-token is consumed once (V008 family
  rotation). At 256 connections × 30s × ~2k QPS ≈ 15k refreshes per level. The
  default pool is 20k RTs; use `--reseed` to refresh before each level. When
  a thread exhausts its slice, it stops (socket errors visible in the summary).

## Layout

```
benchmarks/
├── README.md                  ← you are here
├── fulla/
│   ├── setup.sh               # boot stack + health gate + seed + token gen + warmup
│   ├── teardown.sh            # stop + (default) remove volumes
│   ├── run-scenario.sh        # staircase runner: warmup → measure → JSON
│   ├── measure-cold-start.sh  # cold-start time + RSS peak (M3)
│   ├── scenarios/
│   │   ├── s1-discovery.lua
│   │   ├── s2-client-credentials.lua
│   │   ├── s3-introspect.lua          # M2: active-token introspection
│   │   ├── s4-auth-code.lua           # M2: multi-step login→token + PKCE
│   │   ├── s5-refresh-token.lua       # M3: one-shot RT pool (use --reseed)
│   │   └── s6-userinfo.lua            # M3: bearer userinfo
│   ├── lib/
│   │   ├── gen-tokens.py              # token + PKCE generation tool
│   │   ├── token-pool.lua             # wrk helper: load + slice token files
│   │   ├── user-pool.lua              # wrk helper: bench user rotation
│   │   ├── result-schema.md           # the JSON emitted into results/
│   │   └── generated/                 # gitignored: access_tokens, refresh_tokens, pkce_pairs
│   └── observe/
│       ├── docker-stats.sh            # container CPU/RSS sampler (M3)
│       └── scrape-metrics.sh          # /metrics poller (M3)
├── reporting/
│   ├── parse-wrk.py           # wrk stdout → structured JSON
│   └── gen-comparison.py      # four-product JSONs → COMPARISON.md (stdout)
├── competitors/               # Phase 0.5: Keycloak / Ory Hydra / Zitadel
│   ├── run-comparison.sh      # serial four-product orchestrator (AC3)
│   ├── run-fulla-session.sh # Fulla same-session rerun (§5.1)
│   ├── run-gc-jitter.sh       # 5-min P99 time series (D6)
│   ├── measure-cold-start.sh  # generic competitor cold start
│   ├── keycloak/ ory/ zitadel/  # compose + setup + run-all + scenarios
│   └── results/               # competitor JSONs + COMPARISON.md
└── results/                   # dated per-level JSON results (committed)
```
