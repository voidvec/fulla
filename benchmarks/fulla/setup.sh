#!/usr/bin/env bash
# benchmarks/fulla/setup.sh
#
# Boot the Fulla target stack (postgres + redis + backend) as the benchmark
# target, then gate on /health/ready, apply seed SQL (the stack does NOT
# auto-seed), and validate that the seed data the S1/S2 scenarios depend on is
# reachable. Part of benchmark-facility-design.md M1.
#
# Reuses: deploy/docker/docker-compose.yml (backend:5555 + PG17 + Redis7,
# FULLA_AUTO_MIGRATE=true), paths.env (path single source of truth). Seed SQL
# is applied explicitly via `docker exec ... psql` — see the inline note below
# for why (postgres does not recurse into initdb.d subdirs; the app's
# MigrationRunner runs schema only, never seed).
#
# Usage:
#   bash benchmarks/fulla/setup.sh
#
# Env overrides:
#   TARGET_URL   default http://127.0.0.1:5555 (published port, bridge mode —
#                host networking is NOT viable under Docker Desktop, see
#                docker-compose.bench.yml)
#   KEEP_VOLUME  =1 to skip the clean-volume reset (re-run on existing stack)
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$BENCH_DIR/../.." && pwd)"
TARGET_URL="${TARGET_URL:-http://127.0.0.1:5555}"

# --- source paths.env (same pattern as manage.sh) ---
PATHS_ENV_FILE="$REPO_ROOT/paths.env"
if [ ! -f "$PATHS_ENV_FILE" ]; then
    echo "[setup] ERROR: paths.env not found at $PATHS_ENV_FILE"
    exit 1
fi
set -a
# shellcheck disable=SC1090
source "$PATHS_ENV_FILE"
set +a
COMPOSE_FILE_ABS="$REPO_ROOT/$COMPOSE_FILE_REL"

# cd to repo root before any docker compose call: build-context relative paths
# in the compose file (e.g. context: ../..) resolve against the CWD, not against
# --project-directory, so a wrong CWD makes buildx bake look in the wrong place
# (e.g. lstat /home/<user>/deploy). manage.sh does the same (cd "$SCRIPT_DIR").
cd "$REPO_ROOT"

echo "[setup] compose file: $COMPOSE_FILE_ABS"

# --- #45: layer an absolute-path override for compose v5.3.1's buildx-bake
# relative-path resolution bug. Generated from the real compose file so it
# tracks service/mount changes; harmless on unaffected compose versions.
# The override is only strictly needed by `up`/`build`, but we attach it to
# every compose invocation here for uniformity.
OVERRIDE_FILE="$(bash "$REPO_ROOT/scripts/docker/compose-override.sh" "$COMPOSE_FILE_ABS")"
COMPOSE_ARGS=(-f "$COMPOSE_FILE_ABS")
[ -n "$OVERRIDE_FILE" ] && [ -f "$OVERRIDE_FILE" ] && COMPOSE_ARGS+=(-f "$OVERRIDE_FILE")
trap 'rm -f "$OVERRIDE_FILE"' EXIT

# --- bench overlay: PG instance tuning (quick-win profile,
# noncode-performance-optimization.md §四/§八). Every compose invocation below
# uses COMPOSE_ARGS, so down/up/seed all see the same topology. (Host
# networking was evaluated and rejected for this environment — see the note
# in docker-compose.bench.yml.)
BENCH_COMPOSE_FILE="$BENCH_DIR/docker-compose.bench.yml"
if [ -f "$BENCH_COMPOSE_FILE" ]; then
    COMPOSE_ARGS+=(-f "$BENCH_COMPOSE_FILE")
    echo "[setup] bench overlay active: PG instance tuning (-c flags, see docker-compose.bench.yml)"
fi

# --- benchmark config: swap config.json → config.bench.json ---
# The compose stack bind-mounts apps/server/config/config.json into the container.
# For benchmarks we need the perf profile (PG pool 64 + Redis pool 64 — pool sweep
# 25/64/100 showed +40-48% QPS at c>=64 going 25→64 and no gain at 100; cache-on
# moved reads to Redis where 20 conns queued badly at c>=32 — plus micro-opts).
# Rather than modifying the dev config, we temporarily swap it: back up
# config.json, copy config.bench.json over it. The swap persists until
# teardown.sh restores it.
BENCH_CONFIG="$REPO_ROOT/$FULLA_SERVER_DIR/config/config.bench.json"
DEV_CONFIG="$REPO_ROOT/$FULLA_SERVER_DIR/config/config.json"
DEV_CONFIG_BACKUP="$REPO_ROOT/$FULLA_SERVER_DIR/config/config.json.dev-backup"
if [ -f "$BENCH_CONFIG" ] && [ ! -f "$DEV_CONFIG_BACKUP" ]; then
    cp "$DEV_CONFIG" "$DEV_CONFIG_BACKUP"
    cp "$BENCH_CONFIG" "$DEV_CONFIG"
    export BENCH_TARGET_CONFIG="config.bench.json"
    echo "[setup] using benchmark config (config.bench.json: PG=64, Redis=64, cache=ON) — config.json backed up, run teardown.sh to restore"
fi

# --- clean volume for determinism (skip if KEEP_VOLUME=1) ---
if [ "${KEEP_VOLUME:-0}" != "1" ]; then
    echo "[setup] resetting volumes (docker compose down -v) for schema/seed determinism..."
    docker compose "${COMPOSE_ARGS[@]}" --project-directory "$REPO_ROOT" down -v \
        --remove-orphans >/dev/null 2>&1 || true
fi

# --- boot the benchmark target (backend + postgres + redis only) ---
# Only the backend, its PG and Redis deps are load-tested. The admin/frontend
# SPAs and Prometheus are irrelevant to backend throughput (design D1/N4) and
# are deliberately NOT started — this also avoids building their (slow, separate)
# images. Same service-subset pattern as scripts/backend/full-test-docker.sh:55.
echo "[setup] bringing up backend + postgres + redis (docker compose up -d)..."
docker compose "${COMPOSE_ARGS[@]}" --project-directory "$REPO_ROOT" up -d \
    fulla-postgres fulla-redis fulla-backend

# --- poll /health/ready until 200 or timeout ---
# postgres storage mode requires DB + Redis both reachable for /health/ready
# to return 200 (HealthController.cc:70-170). This is the real readiness gate.
echo "[setup] waiting for $TARGET_URL/health/ready ..."
READY=0
for i in $(seq 1 60); do
    CODE="$(curl -s -o /dev/null -w '%{http_code}' "$TARGET_URL/health/ready" 2>/dev/null || echo 000)"
    if [ "$CODE" = "200" ]; then
        READY=1
        echo "[setup] ready after ${i} polls (~$((i*2))s)"
        break
    fi
    sleep 2
done
if [ "$READY" != "1" ]; then
    echo "[setup] ERROR: target did not become ready within ~120s (last code: $CODE)"
    echo "[setup] dumping backend logs (last 40 lines):"
    docker compose "${COMPOSE_ARGS[@]}" --project-directory "$REPO_ROOT" logs --tail=40 fulla-backend 2>/dev/null || true
    exit 1
fi

# --- apply seed data (NOT auto-loaded by the stack) ---
# IMPORTANT: the docker-compose stack does NOT auto-seed. The postgres entrypoint
# mounts seed/*.sql under initdb.d/seed/, but postgres does NOT recurse into
# subdirectories (compose comment docker-compose.yml:79-81 calls this a no-op),
# and the app's MigrationRunner (FULLA_AUTO_MIGRATE=true) runs schema migrations
# ONLY, never seed. Seed data (dev clients from apps/server/seed + the 512
# bench users from benchmarks/fulla/seed — benchmark-only, not shipped with the
# server image) must be applied explicitly via psql — same pattern as
# scripts/backend/setup-database.sh and deploy/docker/docker-quick-verify-debug.sh:62-64.
#
# Race note: MigrationRunner runs migrations on a detached thread with a 500ms
# startup delay (MigrationRunner.cc:82-91), so /health/ready returning 200 only
# proves the DB is reachable, NOT that migrations finished. Seed SQL targets
# migration-created tables (oauth2_clients, users, ...), so we retry the whole
# seed bundle until it applies cleanly. All seed files are idempotent
# (ON CONFLICT DO NOTHING), so retries are safe.
SEED_DIR_HOST="$REPO_ROOT/$FULLA_SERVER_DIR/seed"
BENCH_SEED_DIR_HOST="$REPO_ROOT/benchmarks/fulla/seed"
echo "[setup] applying seed SQL from $SEED_DIR_HOST + $BENCH_SEED_DIR_HOST ..."
PG_CONTAINER="$(docker compose "${COMPOSE_ARGS[@]}" --project-directory "$REPO_ROOT" ps -q fulla-postgres 2>/dev/null || true)"
if [ -z "$PG_CONTAINER" ]; then
    echo "[setup] ERROR: could not resolve the postgres container to apply seed."
    exit 1
fi
SEED_APPLIED=0
for attempt in $(seq 1 30); do
    SEED_FAIL=0
    for seed_sql in "$SEED_DIR_HOST"/*.sql "$BENCH_SEED_DIR_HOST"/*.sql; do
        fname="$(basename "$seed_sql")"
        if ! docker exec -i "$PG_CONTAINER" \
            psql -U fulla_user -d fulla_db -v ON_ERROR_STOP=1 -q \
            < "$seed_sql" >/dev/null 2>&1; then
            SEED_FAIL=$((SEED_FAIL+1))
        fi
    done
    if [ "$SEED_FAIL" -eq 0 ]; then
        SEED_APPLIED=1
        echo "[setup] seed applied on attempt $attempt ($(ls "$SEED_DIR_HOST"/*.sql "$BENCH_SEED_DIR_HOST"/*.sql | wc -l) files)"
        break
    fi
    # migrations not finished yet — wait and retry
    sleep 2
done
if [ "$SEED_APPLIED" != "1" ]; then
    echo "[setup] ERROR: seed did not apply cleanly within ~60s ($SEED_FAIL file(s) failing; likely migrations unfinished)."
    echo "[setup] dumping backend migration logs (last 30 lines):"
    docker compose "${COMPOSE_ARGS[@]}" --project-directory "$REPO_ROOT" logs --tail=30 fulla-backend 2>/dev/null || true
    exit 1
fi

# --- seed validation: confirm S2 client_credentials is reachable ---
# backend-svc is seeded by dev_backend_client.sql (applied above). A 200 +
# access_token proves the token endpoint + RS256 signing + postgres client
# lookup are all wired.
#
# Uses HTTP Basic auth (not body-post secret): the seeded backend-svc declares
# token_endpoint_auth_method=client_secret_basic, so F-017 rejects a body-posted
# client_secret with 401 invalid_client. Same form as s2-client-credentials.lua.
#
# Scope: the legacy 'read'/'write' scopes were dropped in #43 — backend-svc
# carries the resource-scope vocabulary (tokens:read, ...), matching the S2
# scenario script.
echo "[setup] validating seed: POST /oauth2/token (client_credentials, backend-svc, HTTP Basic)..."
SEED_RESP="$(curl -s -X POST "$TARGET_URL/oauth2/token" \
    -H "Content-Type: application/x-www-form-urlencoded" \
    -H "Authorization: Basic YmFja2VuZC1zdmM6dGVzdC1zZWNyZXQ=" \
    -d "grant_type=client_credentials&scope=tokens:read" 2>/dev/null || true)"
AT="$(python3 -c "import sys,json; print(json.loads(sys.argv[1]).get('access_token',''))" "$SEED_RESP" 2>/dev/null || true)"
if [ -z "$AT" ]; then
    echo "[setup] ERROR: seed validation failed — no access_token in response:"
    echo "        $SEED_RESP"
    exit 1
fi
echo "[setup] seed OK: client_credentials token issued (len=${#AT})"

# --- discovery endpoint smoke (S1 dependency) ---
DISC_CODE="$(curl -s -o /dev/null -w '%{http_code}' "$TARGET_URL/.well-known/openid-configuration" 2>/dev/null || echo 000)"
if [ "$DISC_CODE" != "200" ]; then
    echo "[setup] WARN: /.well-known/openid-configuration returned $DISC_CODE (S1 may fail)"
fi
JWKS_CODE="$(curl -s -o /dev/null -w '%{http_code}' "$TARGET_URL/.well-known/jwks.json" 2>/dev/null || echo 000)"
if [ "$JWKS_CODE" != "200" ]; then
    echo "[setup] WARN: /.well-known/jwks.json returned $JWKS_CODE (S1 may fail)"
fi

# --- warmup_bench_users function (defined here, called after token gen) ---
# M1 scenarios (S1 discovery, S2 client_credentials) do not consume the bench
# users, but the seed is in place so M2's auth_code scenario can use them
# without a schema change. The first login of each legacy-hash user triggers a
# PBKDF2 rehash (AuthService.cc:94-122); we warm up BEFORE timed runs so that
# CPU cost lands in warmup, not measured throughput.
#
# S256 PKCE is REQUIRED here (F-011 / RFC 9700 §2.1.1): fulla-portal is PUBLIC and
# require_pkce_for_public=true in all shipped configs, so a login without
# code_challenge is rejected. Each user gets its own code_verifier + the derived
# S256 code_challenge.
warmup_bench_users() {
    local n="${1:-512}"
    echo "[setup] warming up $n bench users (PBKDF2 rehash + PKCE, first login)..."
    local ok=0 fail=0
    for i in $(seq 0 $((n-1))); do
        local uname verifier challenge code login_code
        uname="$(printf 'bench_user_%04d' "$i")"
        # generate a random S256 PKCE pair (43-128 char verifier; base64url of 32 random bytes)
        read -r verifier challenge <<EOF
$(python3 -c "
import os, base64, hashlib
v = base64.urlsafe_b64encode(os.urandom(32)).rstrip(b'=').decode()
c = base64.urlsafe_b64encode(hashlib.sha256(v.encode()).digest()).rstrip(b'=').decode()
print(v, c)
")"
EOF
        # check the HTTP status, not just curl's exit code (curl returns 0 for HTTP 401/500)
        login_code="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$TARGET_URL/oauth2/login" \
            -d "username=${uname}&password=admin&client_id=fulla-portal&redirect_uri=http://127.0.0.1:5173/callback&scope=openid+profile&state=warmup-${i}&code_challenge=${challenge}&code_challenge_method=S256&json=true" \
            2>/dev/null || echo 000)"
        if [ "$login_code" = "200" ]; then
            ok=$((ok+1))
        else
            fail=$((fail+1))
        fi
    done
    echo "[setup] warmup: $ok ok, $fail failed"
    if [ "$fail" -gt 0 ]; then
        echo "[setup] WARN: $fail users failed warmup — check F-011 PKCE / seed / lockout"
    fi
}

# --- M2+ token pool generation (S3 introspect, S5 refresh, S6 userinfo) ---
# Scenarios S3/S5/S6 require pre-seeded tokens so wrk can drive them at high
# concurrency without the token-issuance step polluting the measured path.
# gen-tokens.py generates raw tokens + their SHA256 hashes (matching
# TokenCrypto.cc:26-37: toUpperCase(sha256Hex(raw))) + PKCE pairs for S4.
# Output goes to lib/generated/ (gitignored). Skip with SKIP_TOKEN_GEN=1.
GEN_TOKENS="$BENCH_DIR/lib/gen-tokens.py"
GENERATED_DIR="$BENCH_DIR/lib/generated"
if [ "${SKIP_TOKEN_GEN:-0}" != "1" ]; then
    echo "[setup] generating benchmark token pools (S3/S5/S6) + PKCE pairs (S4)..."
    # rt-count must exceed the consumption ceiling of the whole staircase,
    # else the pool itself caps throughput: at 20000 RTs a 10s measure window
    # tops out at 2,000 QPS regardless of server capacity (the historical
    # "S5 saturates at 2k" artifact — every S5 level showed total_requests
    # ~= 19999). 60000 gives 6k-QPS headroom; S5 still needs --reseed per
    # level (one-shot rotation).
    python3 "$GEN_TOKENS" all --at-count 2000 --rt-count 60000 --pkce-count 512

    # Generate bench_users.txt (username list for user-pool.lua)
    python3 -c "
import pathlib
out = pathlib.Path(r'$GENERATED_DIR') / 'bench_users.txt'
out.parent.mkdir(parents=True, exist_ok=True)
lines = [f'bench_user_{i:04d}\n' for i in range(512)]
out.write_text(''.join(lines), encoding='utf-8')
print(f'[setup] wrote {len(lines)} usernames to {out}')
"

    # Apply the token seed SQL (idempotent ON CONFLICT DO NOTHING).
    # These go into oauth2_access_tokens / oauth2_refresh_tokens.
    for tok_sql in "$GENERATED_DIR"/bench_access_tokens.sql "$GENERATED_DIR"/bench_refresh_tokens.sql; do
        if [ -f "$tok_sql" ]; then
            fname="$(basename "$tok_sql")"
            if docker exec -i "$PG_CONTAINER" \
                psql -U fulla_user -d fulla_db -v ON_ERROR_STOP=1 -q \
                < "$tok_sql" >/dev/null 2>&1; then
                echo "[setup] token seed applied: $fname"
            else
                echo "[setup] WARN: $fname failed to apply — S3/S5/S6 may fail"
            fi
        fi
    done
fi

# --- warmup bench users (PBKDF2 rehash — M2+ scenarios need this) ---
# The first login of each legacy-hash bench user triggers a PBKDF2 rehash
# (AuthService.cc:94-122), which is CPU-intensive. We warm up all 512 users
# here so that cost lands in setup, not in measured S4 throughput.
# Skip with SKIP_WARMUP=1.
#
# warmup_bench_users() is defined above (before the discovery smoke check) and
# must be, because bash requires a function definition to precede its call.
if [ "${SKIP_WARMUP:-0}" != "1" ]; then
    warmup_bench_users 512
fi

echo "[setup] done. target=$TARGET_URL  (bench users + token pools ready for S1–S6)"
