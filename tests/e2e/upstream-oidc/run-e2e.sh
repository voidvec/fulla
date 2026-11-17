#!/usr/bin/env bash
# P2-1b: REAL-upstream OIDC-in federation E2E (Keycloak).
#
# Boots Keycloak 26 (start-dev) + the fulla server against the local test
# postgres/redis, provisions a realm/client/user, registers the provider in
# the fulla registry, and drives the complete browser-shaped flow with curl:
#
#   authorize dance -> Keycloak login form POST -> code capture ->
#   POST /api/auth/oidc/{provider}/login -> first-party token pair ->
#   GET /oauth2/userinfo (claims from Keycloak userinfo)
#
# The upstream client credential and the test-user password are generated
# at runtime from /dev/urandom and passed through $WORK files — nothing
# credential-like is ever committed, matching the repo's "throwaway local
# bench stack" convention.
#
# Usage:
#   bash tests/e2e/upstream-oidc/run-e2e.sh
# Env:
#   FULLA_SERVER_BIN  path to the built fulla-server (default: auto-detect
#                     build/linux-release under the repo)
#   PG_CONTAINER/PG_PORT/REDIS_CONTAINER/REDIS_PORT   infra (defaults:
#                     fulla-model-pg / 55432 / fulla-diag-redis / 56379)
#   KC_CONTAINER/KC_PORT                             Keycloak (kc-e2e / 18080)
#   KEEP_STACK=1     leave containers + server running (dev loop)
#   SKIP_PROVISION=1 realm already provisioned (dev loop)
#
# Exit codes: 0 pass; 1 assertion/setup failure.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"

rand_token() { head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n'; }

PG_CONTAINER="${PG_CONTAINER:-fulla-model-pg}"
PG_PORT="${PG_PORT:-55432}"
REDIS_CONTAINER="${REDIS_CONTAINER:-fulla-diag-redis}"
REDIS_PORT="${REDIS_PORT:-56379}"
KC_CONTAINER="${KC_CONTAINER:-kc-e2e}"
KC_PORT="${KC_PORT:-18080}"
# Override for networks that cannot reach quay.io (e.g. local mirror
# proxies); CI runners use the default. 26.7.1 matches the version the
# competitor benchmark baseline ran (same image, cached locally there).
KEYCLOAK_IMAGE="${KEYCLOAK_IMAGE:-quay.io/keycloak/keycloak:26.7.1}"
REALM="e2e"
KC_CLIENT_ID="fulla-e2e"
KC_USER="e2e-user"
KC_EMAIL="e2e@example.com"
PROVIDER_KEY="kc-e2e"
ORIGIN="http://localhost:5173"
REDIRECT_URI="$ORIGIN/callback/$PROVIDER_KEY"

if [[ -z "${FULLA_SERVER_BIN:-}" ]]; then
  for candidate in build/linux-release/apps/server/fulla-server \
                   build/linux-release/apps/server/Debug/fulla-server; do
    if [[ -x "$candidate" ]]; then FULLA_SERVER_BIN="$candidate"; break; fi
  done
fi
if [[ -z "${FULLA_SERVER_BIN:-}" || ! -x "$FULLA_SERVER_BIN" ]]; then
  echo "FAIL: fulla-server binary not found (set FULLA_SERVER_BIN)"; exit 1
fi
FULLA_SERVER_BIN="$(cd "$(dirname "$FULLA_SERVER_BIN")" && pwd)/$(basename "$FULLA_SERVER_BIN")"

# 127.0.0.1 (not "localhost"): the server-side HttpClient resolves
# localhost to ::1 first, and the published container port is IPv4-only.
KC_URL="http://127.0.0.1:$KC_PORT"
ISSUER="$KC_URL/realms/$REALM"

WORK="$(mktemp -d /tmp/fulla-oidc-e2e.XXXXXX)"
SERVER_PID=""
FAILED=0

log()  { printf '\033[1;34m[e2e]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[e2e][FAIL]\033[0m %s\n' "$*"; FAILED=1; }
die()  { fail "$@"; cleanup; exit 1; }

cleanup() {
  if [[ -n "$SERVER_PID" ]]; then kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; fi
  if [[ -z "${KEEP_STACK:-}" ]]; then
    docker rm -f "$KC_CONTAINER" >/dev/null 2>&1 || true
    rm -rf "$WORK"
  else
    log "KEEP_STACK: work dir $WORK (server stopped, containers kept)"
  fi
}
trap cleanup EXIT

ensure_infra() {
  docker exec "$PG_CONTAINER" pg_isready -U fulla_user -d fulla_db >/dev/null 2>&1 \
    || die "postgres container $PG_CONTAINER not reachable"
  docker exec "$REDIS_CONTAINER" redis-cli -a 123456 ping >/dev/null 2>&1 \
    || die "redis container $REDIS_CONTAINER not reachable"
  # Fresh schema + dev seeds (idempotent chain).
  docker exec -i "$PG_CONTAINER" psql -U fulla_user -d postgres -q \
    -c "DROP DATABASE IF EXISTS fulla_db" -c "CREATE DATABASE fulla_db" >/dev/null 2>&1
  for f in apps/server/migrations/V*.sql apps/server/seed/*.sql; do
    docker exec -i "$PG_CONTAINER" psql -U fulla_user -d fulla_db -q -v ON_ERROR_STOP=1 < "$f" >/dev/null 2>&1 \
      || die "apply failed: $f"
  done
  log "infra ready (pg:$PG_PORT redis:$REDIS_PORT)"
}

boot_keycloak() {
  docker rm -f "$KC_CONTAINER" >/dev/null 2>&1 || true
  docker run -d --name "$KC_CONTAINER" -p "$KC_PORT:8080" \
    -e KEYCLOAK_ADMIN=admin -e KEYCLOAK_ADMIN_PASSWORD=admin \
    "$KEYCLOAK_IMAGE" start-dev >/dev/null 2>&1 \
    || die "keycloak container failed to start (image: $KEYCLOAK_IMAGE)"
  # Wait on the MASTER realm (the e2e realm only exists after provisioning).
  for _ in $(seq 1 60); do
    curl -fsS "$KC_URL/realms/master/.well-known/openid-configuration" >/dev/null 2>&1 && break
    sleep 2
  done
  curl -fsS "$KC_URL/realms/master/.well-known/openid-configuration" >/dev/null 2>&1 \
    || { docker logs "$KC_CONTAINER" 2>&1 | tail -5; die "keycloak not reachable at $KC_URL"; }
  log "keycloak up at $KC_URL"
}

# kcadm access: docker exec + binary as argv prefix; the connection flags
# must come AFTER the subcommand (kcadm argument parsing).
KCADM_PREFIX=(docker exec "$KC_CONTAINER" /opt/keycloak/bin/kcadm.sh)
KCADM_CONN=(--server "http://localhost:8080" --realm master --user admin --password admin)

provision_realm() {
  [[ -n "${SKIP_PROVISION:-}" ]] && { log "realm provisioning skipped"; return; }
  # Runtime-generated throwaway credentials, kept in $WORK files only.
  rand_token > "$WORK/kc-client-cred"
  printf 'pass-%s' "$(rand_token)" > "$WORK/kc-user-pass"
  local client_cred user_pass
  client_cred="$(cat "$WORK/kc-client-cred")"
  user_pass="$(cat "$WORK/kc-user-pass")"

  "${KCADM_PREFIX[@]}" create realms "${KCADM_CONN[@]}" \
    -s realm="$REALM" -s enabled=true >/dev/null 2>&1 \
    || die "create realm"
  "${KCADM_PREFIX[@]}" create clients "${KCADM_CONN[@]}" -r "$REALM" \
    -s "clientId=$KC_CLIENT_ID" -s enabled=true -s "secret=$client_cred" \
    -s "redirectUris=[\"$REDIRECT_URI\"]" -s "webOrigins=[\"+\"]" -i \
    >/dev/null 2>&1 || die "create client"
  "${KCADM_PREFIX[@]}" create users "${KCADM_CONN[@]}" -r "$REALM" \
    -s username="$KC_USER" -s enabled=true -s email="$KC_EMAIL" -s emailVerified=true \
    -s firstName=E2E -s lastName=User \
    >/dev/null 2>&1 || die "create user"
  "${KCADM_PREFIX[@]}" set-password "${KCADM_CONN[@]}" -r "$REALM" \
    --username "$KC_USER" --new-password "$user_pass" \
    >/dev/null 2>&1 || die "set user password"
  curl -fsS "$ISSUER/.well-known/openid-configuration" >/dev/null 2>&1 \
    || die "e2e realm discovery missing after provisioning"
  log "realm provisioned (client=$KC_CLIENT_ID user=$KC_USER)"
}

start_server() {
  cp apps/server/config/config.json "$WORK/config.json"
  python3 - "$WORK/config.json" "$PG_PORT" "$REDIS_PORT" <<'PY'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1])
cfg = json.loads(p.read_text())
cfg["db_clients"][0]["port"] = int(sys.argv[2])
cfg["redis_clients"][0]["port"] = int(sys.argv[3])
p.write_text(json.dumps(cfg, indent=4))
PY
  ( cd "$WORK" && exec "$FULLA_SERVER_BIN" ) > "$WORK/server.log" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 1 30); do
    curl -fsS http://127.0.0.1:5555/health/ready >/dev/null 2>&1 && break
    sleep 1
  done
  curl -fsS http://127.0.0.1:5555/health/ready >/dev/null 2>&1 \
    || { tail -5 "$WORK/server.log"; die "fulla server not ready"; }
  log "fulla server up (pid $SERVER_PID)"
}

json_field() { # json_field <file> <python-expr over j>
  python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$1" "$2"
}

register_provider() {
  # Admin login through the real PKCE flow (same recipe as the test suite).
  python3 - <<'PY' > "$WORK/pkce.txt"
import base64, hashlib, os
v = base64.urlsafe_b64encode(os.urandom(32)).rstrip(b"=").decode()
c = base64.urlsafe_b64encode(hashlib.sha256(v.encode()).digest()).rstrip(b"=").decode()
print(v); print(c)
PY
  CODE_VERIFIER="$(sed -n 1p "$WORK/pkce.txt")"
  CODE_CHALLENGE="$(sed -n 2p "$WORK/pkce.txt")"

  curl -fsS -X POST "http://127.0.0.1:5555/oauth2/login?json=true" \
    -H "content-type: application/json" \
    -d '{"username":"admin","password":"admin","client_id":"fulla-admin-console","redirect_uri":"http://127.0.0.1:5174/admin/callback","scope":"openid profile admin","code_challenge":"'"$CODE_CHALLENGE"'","code_challenge_method":"S256"}' \
    > "$WORK/admin-login.json" 2>/dev/null || die "admin login leg 1 failed"
  local code
  code="$(json_field "$WORK/admin-login.json" 'j.get("code","")')"
  [[ -n "$code" ]] || die "admin login: no code (body: $(cat "$WORK/admin-login.json"))"
  curl -sS -X POST http://127.0.0.1:5555/oauth2/token \
    -H "content-type: application/x-www-form-urlencoded" \
    --data-urlencode "grant_type=authorization_code" \
    --data-urlencode "code=$code" \
    --data-urlencode "redirect_uri=http://127.0.0.1:5174/admin/callback" \
    --data-urlencode "client_id=fulla-admin-console" \
    --data-urlencode "client_secret=" \
    --data-urlencode "code_verifier=$CODE_VERIFIER" \
    > "$WORK/admin-token.json"
  if ! python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$WORK/admin-token.json" 2>/dev/null \
     || ! grep -q access_token "$WORK/admin-token.json"; then
    die "admin token leg failed: $(cat "$WORK/admin-token.json")"
  fi

  local bearer client_cred
  bearer="$(json_field "$WORK/admin-token.json" 'j["access_token"]')"
  client_cred="$(cat "$WORK/kc-client-cred")"
  curl -fsS -X POST http://127.0.0.1:5555/api/admin/upstream-providers \
    -H "authorization: Bearer $bearer" -H "content-type: application/json" \
    -d "{\"provider_key\":\"$PROVIDER_KEY\",\"display_name\":\"Keycloak E2E\",\"issuer\":\"$ISSUER\",\"client_id\":\"$KC_CLIENT_ID\",\"client_secret\":\"$client_cred\"}" \
    > "$WORK/provider.json" || die "provider registration failed"
  log "provider registered: $(json_field "$WORK/provider.json" 'j["provider_key"]')"
}

authorize_transaction() {
  local path="/api/auth/providers/$PROVIDER_KEY/authorize"
  # 1st call: binding cookie.
  curl -fsS -X POST "http://127.0.0.1:5555$path" -H "origin: $ORIGIN" \
    -c "$WORK/jar" -o /dev/null -w '%{http_code}' > "$WORK/authz1.code" || true
  [[ "$(cat "$WORK/authz1.code")" == "204" ]] || die "authorize leg1: $(cat "$WORK/authz1.code")"
  # 2nd call: transaction.
  curl -fsS -X POST "http://127.0.0.1:5555$path" -H "origin: $ORIGIN" \
    -b "$WORK/jar" -c "$WORK/jar" > "$WORK/authz.json" || die "authorize leg2 failed"
  STATE="$(json_field "$WORK/authz.json" 'j["state"]')"
  AUTHZ_URL="$(json_field "$WORK/authz.json" 'j["authorize_url"]')"
  [[ ${#STATE} -eq 64 ]] || die "bad state: $STATE"
  [[ "$AUTHZ_URL" == "$ISSUER/protocol/openid-connect/auth"* ]] || die "authorize_url not keycloak: $AUTHZ_URL"
  log "authorize transaction issued (state=${STATE:0:8}...)"
}

keycloak_login() {
  curl -fsS -c "$WORK/kcjar" "$AUTHZ_URL" > "$WORK/login.html" || die "keycloak login page fetch failed"
  ACTION="$(python3 - "$WORK/login.html" <<'PY'
import re, sys, html
m = re.search(r'<form[^>]+action="([^"]+)"', open(sys.argv[1], encoding="utf-8").read())
print(html.unescape(m.group(1)) if m else "")
PY
)"
  [[ -n "$ACTION" ]] || die "no login form action in keycloak page"
  local user_pass
  user_pass="$(cat "$WORK/kc-user-pass")"
  # POST credentials; the 302 Location carries ?code=...&state=...
  curl -fsS -b "$WORK/kcjar" -c "$WORK/kcjar" -o /dev/null \
    -w '%{redirect_url}' \
    --data-urlencode "username=$KC_USER" \
    --data-urlencode "password=$user_pass" \
    --data-urlencode "credentialId=" \
    "$ACTION" > "$WORK/redirect.txt" || die "keycloak login POST failed"
  CODE="$(python3 - "$WORK/redirect.txt" <<'PY'
import sys, urllib.parse as up
loc = open(sys.argv[1]).read().strip()
q = up.parse_qs(up.urlparse(loc).query)
print(q.get("code", [""])[0])
PY
)"
  [[ -n "$CODE" ]] || die "no code in redirect: $(cat "$WORK/redirect.txt")"
  log "upstream code captured (${CODE:0:8}...)"
}

exchange_and_verify() {
  curl -fsS -X POST "http://127.0.0.1:5555/api/auth/oidc/$PROVIDER_KEY/login" \
    -H "origin: $ORIGIN" -H "content-type: application/json" \
    -b "$WORK/jar" -c "$WORK/jar" \
    -d "{\"code\":\"$CODE\",\"state\":\"$STATE\"}" > "$WORK/tokens.json" \
    || die "login exchange failed"
  ACCESS="$(json_field "$WORK/tokens.json" 'j["access_token"]')"
  [[ -n "$ACCESS" ]] || die "no access_token in exchange response"
  log "first-party token pair issued"

  curl -fsS http://127.0.0.1:5555/oauth2/userinfo \
    -H "authorization: Bearer $ACCESS" > "$WORK/userinfo.json" || die "userinfo failed"
  SUB="$(json_field "$WORK/userinfo.json" 'j.get("sub","")')"
  EMAIL="$(json_field "$WORK/userinfo.json" 'j.get("email","")')"
  [[ -n "$SUB" ]] || fail "userinfo: empty sub"
  [[ "$EMAIL" == "$KC_EMAIL" ]] || fail "userinfo: email '$EMAIL' != $KC_EMAIL (claims mapping)"
  log "userinfo: sub=${SUB:0:8}... email=$EMAIL"

  # Login-page listing advertises the registry provider.
  curl -fsS http://127.0.0.1:5555/api/auth/providers > "$WORK/providers.json" || die "provider listing failed"
  python3 - "$WORK/providers.json" "$PROVIDER_KEY" <<'PY' || fail "provider listing missing registry entry"
import json, sys
j = json.load(open(sys.argv[1]))
assert any(p.get("provider") == sys.argv[2] and p.get("type") == "oidc" for p in j["providers"]), \
    "registry provider not advertised"
PY
  log "login page advertises $PROVIDER_KEY (type=oidc)"
}

main() {
  ensure_infra
  boot_keycloak
  provision_realm
  start_server
  register_provider
  authorize_transaction
  keycloak_login
  exchange_and_verify
  if [[ "$FAILED" -ne 0 ]]; then die "one or more assertions failed"; fi
  printf '\033[1;32m[e2e] PASS\033[0m — Keycloak federated login E2E complete\n'
}

main "$@"
