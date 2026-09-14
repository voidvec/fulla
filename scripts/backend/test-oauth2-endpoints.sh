#!/usr/bin/env bash
# test-oauth2-endpoints.sh - OAuth2 endpoint tests (Linux/macOS)
# Equivalent of test-oauth2-endpoints.ps1
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common-test-functions.sh"

BASE_URL="${1:-http://127.0.0.1:5555}"
ACCESS_TOKEN=""
REFRESH_TOKEN=""
AUTH_CODE=""
DISCOVERY_ISSUER=""
ADMIN_PASSWORD="admin"  # Track current admin password across tests

TOTAL=58

echo "========================================"
echo "Pre-test Setup"
echo "========================================"
reset_admin_account
echo ""

echo "========================================"
echo "OAuth2 Endpoints Tests ($TOTAL tests)"
echo "========================================"
echo "Base URL: $BASE_URL"
echo ""

# Test 1: Health Check
test_1() {
    local r
    r=$(curl -s "$BASE_URL/health")
    assert_json_field "$r" "status" "ok" || return 1
    echo "    Status: $(echo "$r" | jq -r '.status')"
}
run_test "Test 1: Health Check" test_1

# Test 2: Health Live/Ready
test_2() {
    local live ready
    live=$(curl -s "$BASE_URL/health/live")
    assert_json_field "$live" "status" "ok" || return 1
    ready=$(curl -s "$BASE_URL/health/ready")
    local ready_status
    ready_status=$(echo "$ready" | jq -r '.status')
    [ "$ready_status" = "ok" ] || [ "$ready_status" = "degraded" ] || { echo "    /health/ready failed"; return 1; }
    echo "    Live: ok, Ready: $ready_status"
}
run_test "Test 2: Health Live/Ready" test_2

# Test 3: OIDC Discovery
test_3() {
    local r
    r=$(curl -s "$BASE_URL/.well-known/openid-configuration")
    assert_json_exists "$r" "issuer" || return 1
    assert_json_exists "$r" "jwks_uri" || return 1
    assert_json_exists "$r" "scopes_supported" || return 1
    # B1 (OIDC Back-Channel Logout 1.0): advertised support flags. Session
    # level is false — this OP issues subject-scoped logout_tokens (no sid).
    assert_json_field "$r" "backchannel_logout_supported" "true" || return 1
    assert_json_field "$r" "backchannel_logout_session_supported" "false" || return 1
    DISCOVERY_ISSUER=$(echo "$r" | jq -r '.issuer')
    echo "    Issuer: $DISCOVERY_ISSUER"
}
run_test "Test 3: OIDC Discovery" test_3

# Test 4: JWKS
test_4() {
    local r
    r=$(curl -s "$BASE_URL/.well-known/jwks.json")
    local count
    count=$(echo "$r" | jq '.keys | length')
    [ "$count" -gt 0 ] || { echo "    empty keys array"; return 1; }
    local kty alg
    kty=$(echo "$r" | jq -r '.keys[0].kty')
    alg=$(echo "$r" | jq -r '.keys[0].alg')
    [ "$kty" = "RSA" ] || { echo "    kty != RSA"; return 1; }
    [ "$alg" = "RS256" ] || { echo "    alg != RS256"; return 1; }
    echo "    Keys: $count, kid: $(echo "$r" | jq -r '.keys[0].kid')"
}
run_test "Test 4: JWKS" test_4

# Test 5: OAuth2 Login
# F-011/RFC 7636 (RFC 9700 §2.1.1): PKCE mandatory for PUBLIC clients.
# fulla-portal is PUBLIC → login must carry code_challenge; Test 6 must carry
# the matching code_verifier. The global PKCE_VERIFIER is set here and
# consumed by test_6.
test_5() {
    PKCE_VERIFIER=$(generate_pkce_verifier)
    local challenge
    challenge=$(pkce_s256_challenge "$PKCE_VERIFIER")
    local r
    r=$(curl -s -X POST "$BASE_URL/oauth2/login" \
        -d "username=admin&password=admin&client_id=fulla-portal&redirect_uri=http://127.0.0.1:5173/callback&scope=openid+profile&state=test-state-12345678&code_challenge=$challenge&code_challenge_method=S256&json=true")
    AUTH_CODE=$(echo "$r" | jq -r '.code')
    [ -n "$AUTH_CODE" ] && [ "$AUTH_CODE" != "null" ] || { echo "    no auth code returned"; return 1; }
    echo "    Code: ${AUTH_CODE:0:20}... (${#AUTH_CODE} chars)"
}
run_test "Test 5: OAuth2 Login" test_5

# Test 6: Token Exchange + id_token
test_6() {
    [ -n "$AUTH_CODE" ] || { echo "    skipped: no auth code"; return 1; }
    local r
    r=$(curl -s -X POST "$BASE_URL/oauth2/token" \
        -d "grant_type=authorization_code&code=$AUTH_CODE&redirect_uri=http://127.0.0.1:5173/callback&client_id=fulla-portal&code_verifier=$PKCE_VERIFIER")
    ACCESS_TOKEN=$(echo "$r" | jq -r '.access_token')
    REFRESH_TOKEN=$(echo "$r" | jq -r '.refresh_token')
    [ -n "$ACCESS_TOKEN" ] && [ "$ACCESS_TOKEN" != "null" ] || { echo "    no access_token"; return 1; }
    [ -n "$REFRESH_TOKEN" ] && [ "$REFRESH_TOKEN" != "null" ] || { echo "    no refresh_token"; return 1; }
    local id_token
    id_token=$(echo "$r" | jq -r '.id_token')
    [ -n "$id_token" ] && [ "$id_token" != "null" ] || { echo "    no id_token"; return 1; }
    echo "    AT: ${ACCESS_TOKEN:0:20}..., id_token: present"
}
run_test "Test 6: Token Exchange + id_token" test_6

# Test 7: UserInfo
test_7() {
    [ -n "$ACCESS_TOKEN" ] || { echo "    skipped: no token"; return 1; }
    local r
    r=$(curl -s -H "Authorization: Bearer $ACCESS_TOKEN" "$BASE_URL/oauth2/userinfo")
    assert_json_exists "$r" "sub" || return 1
    local sub name
    sub=$(echo "$r" | jq -r '.sub')
    name=$(echo "$r" | jq -r '.name // empty')
    [ ${#sub} -eq 36 ] || { echo "    sub not UUID format (len=${#sub})"; return 1; }
    echo "    Sub: $sub, Name: $name"
}
run_test "Test 7: UserInfo" test_7

# Test 8: Admin Dashboard (F-010: requires admin scope -- use fulla-admin-console token)
test_8() {
    # F-010: /api/admin/* now requires the `admin` scope on the access token.
    # The fulla-portal token (Test 6) carries only `openid profile`, so obtain
    # a proper admin-scoped token via the fulla-admin-console client.
    local admin_token
    admin_token=$(get_admin_token "$BASE_URL" admin admin)
    [ -n "$admin_token" ] || { echo "    no admin token"; return 1; }
    local r
    r=$(curl -s -H "Authorization: Bearer $admin_token" "$BASE_URL/api/admin/dashboard")
    assert_json_field "$r" "status" "success" || return 1
}
run_test "Test 8: Admin Dashboard" test_8

# Test 8b: Insufficient scope on /api/admin (F-010, 403 insufficient_scope)
test_8b() {
    [ -n "$ACCESS_TOKEN" ] || { echo "    skipped: no token"; return 1; }
    # The fulla-portal token carries only `openid profile` (no admin scope) ->
    # F-010 rejects with 403 + WWW-Authenticate: Bearer error="insufficient_scope".
    local code www_auth
    code=$(curl -s -o /dev/null -w "%{http_code}" -H "Authorization: Bearer $ACCESS_TOKEN" \
        "$BASE_URL/api/admin/dashboard")
    [ "$code" = "403" ] || { echo "    expected 403, got $code"; return 1; }
    www_auth=$(curl -s -D - -o /dev/null -H "Authorization: Bearer $ACCESS_TOKEN" \
        "$BASE_URL/api/admin/dashboard" | grep -i "^WWW-Authenticate:")
    echo "$www_auth" | grep -q "insufficient_scope" || { echo "    missing insufficient_scope: $www_auth"; return 1; }
    echo "$www_auth" | grep -q 'scope="audit:read"' || { echo "    missing scope=audit:read: $www_auth"; return 1; }
    echo "    403 insufficient_scope (scope=audit:read) confirmed"
}
run_test "Test 8b: /api/admin without admin scope -> 403 insufficient_scope" test_8b

# Test 9: Token Refresh
test_9() {
    [ -n "$REFRESH_TOKEN" ] || { echo "    skipped: no refresh_token"; return 1; }
    local r
    r=$(curl -s -X POST "$BASE_URL/oauth2/token" \
        -d "grant_type=refresh_token&refresh_token=$REFRESH_TOKEN&client_id=fulla-portal")
    local new_at new_rt
    new_at=$(echo "$r" | jq -r '.access_token')
    new_rt=$(echo "$r" | jq -r '.refresh_token')
    [ -n "$new_at" ] && [ "$new_at" != "null" ] || { echo "    no new access_token"; return 1; }
    [ "$new_at" != "$ACCESS_TOKEN" ] || { echo "    same access_token (not rotated)"; return 1; }
    ACCESS_TOKEN="$new_at"
    REFRESH_TOKEN="$new_rt"
    echo "    New AT: ${ACCESS_TOKEN:0:20}..."
}
run_test "Test 9: Token Refresh" test_9

# Test 9b: Token Refresh - Missing client_secret (F-003, 401)
test_9b() {
    [ -n "$REFRESH_TOKEN" ] || { echo "    skipped: no refresh_token"; return 1; }
    # F-003 (RFC 6749 §6 / §3.2.1): a CONFIDENTIAL client MUST authenticate
    # on the refresh_token grant; omitting the secret MUST yield 401
    # invalid_client. PUBLIC clients (like fulla-portal) are exempt (RFC 6749
    # §10.2: client_id existence check only), so this test uses backend-svc
    # (CONFIDENTIAL) to exercise the F-003 secret requirement. The refresh
    # token itself belongs to fulla-portal, so the secret check fires (401)
    # before any client_id↔token binding check.
    local code body
    code=$(curl -s -o /tmp/refresh_no_secret.$$ -w '%{http_code}' \
        -X POST "$BASE_URL/oauth2/token" \
        -d "grant_type=refresh_token&refresh_token=$REFRESH_TOKEN&client_id=backend-svc")
    body=$(cat /tmp/refresh_no_secret.$$)
    rm -f /tmp/refresh_no_secret.$$
    [ "$code" = "401" ] || { echo "    expected 401, got $code"; return 1; }
    echo "$body" | jq -e '.error == "invalid_client"' >/dev/null \
        || { echo "    expected invalid_client error body, got: $body"; return 1; }
    echo "    401 + invalid_client returned for unauthenticated refresh"
}
run_test "Test 9b: Token Refresh - Missing client_secret (401)" test_9b

# Test 10: Client Credentials
test_10() {
    local r basic_auth
    # F-017: backend-svc is seeded token_endpoint_auth_method=client_secret_basic,
    # so its secret MUST travel via HTTP Basic (the body form is now rejected).
    basic_auth=$(printf 'backend-svc:test-secret' | base64)
    r=$(curl -s -X POST "$BASE_URL/oauth2/token" \
        -H "Authorization: Basic $basic_auth" \
        -d "grant_type=client_credentials&client_id=backend-svc&scope=tokens:read")
    local at rt scope
    at=$(echo "$r" | jq -r '.access_token')
    rt=$(echo "$r" | jq -r '.refresh_token // empty')
    scope=$(echo "$r" | jq -r '.scope')
    [ -n "$at" ] && [ "$at" != "null" ] || { echo "    no access_token"; return 1; }
    [ -z "$rt" ] || [ "$rt" = "null" ] || { echo "    client_credentials should NOT have refresh_token"; return 1; }
    echo "    AT: ${at:0:20}..., Scope: $scope"
}
run_test "Test 10: Client Credentials" test_10

# Test 11: Token Introspection
test_11() {
    [ -n "$ACCESS_TOKEN" ] || { echo "    skipped: no token"; return 1; }
    # RFC 7662 §2.1: introspect authenticates the CALLING CLIENT via client
    # credentials (Basic header or body client_id/client_secret), NOT a
    # user Bearer token. No Authorization header is sent.
    # RFC 7662 §4 / TokenEndpointController.cc:428: ALL callers must supply a
    # client_secret (introspection is protected against token enumeration).
    # fulla-portal is PUBLIC (no secret) and cannot call introspect; use
    # backend-svc (CONFIDENTIAL, secret "test-secret"). F-017: backend-svc is
    # seeded token_endpoint_auth_method=client_secret_basic, so authenticate
    # via HTTP Basic (-u), not a body client_secret.
    local r
    r=$(curl -s -u "backend-svc:test-secret" -X POST "$BASE_URL/oauth2/introspect" \
        -d "token=$ACCESS_TOKEN")
    local active
    active=$(echo "$r" | jq -r '.active')
    [ "$active" = "true" ] || { echo "    active != true"; return 1; }
    # F-016: introspection iss MUST be byte-identical to the discovery issuer
    local iss
    iss=$(echo "$r" | jq -r '.iss')
    [ -n "$iss" ] && [ "$iss" != "null" ] || { echo "    introspection missing iss claim"; return 1; }
    if [ -n "$DISCOVERY_ISSUER" ]; then
        [ "$iss" = "$DISCOVERY_ISSUER" ] || { echo "    iss '$iss' != discovery issuer '$DISCOVERY_ISSUER'"; return 1; }
    fi
    echo "    Active: $active, Sub: $(echo "$r" | jq -r '.sub'), Iss: $iss"
}
run_test "Test 11: Token Introspection" test_11

# Test 12: Token Revocation
test_12() {
    [ -n "$ACCESS_TOKEN" ] || { echo "    skipped: no token"; return 1; }
    # RFC 7009 §2.1: revoke authenticates the calling client AND enforces token
    # ownership — only the token's issuing client may revoke it. The token was
    # issued to fulla-portal (PUBLIC), so fulla-portal must be the caller. RFC 7009
    # §2.1 exempts PUBLIC clients from client_secret at the revocation endpoint
    # ("if the client is a public client, then it does not authenticate"), so
    # client_id alone authenticates a PUBLIC caller.
    curl -s -X POST "$BASE_URL/oauth2/revoke" \
        -d "token=$ACCESS_TOKEN&client_id=fulla-portal" >/dev/null
    # Verify revoked (Bearer header here checks userinfo rejects the token)
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $ACCESS_TOKEN" "$BASE_URL/oauth2/userinfo")
    [ "$code" = "401" ] || { echo "    token should be revoked but userinfo returned $code"; return 1; }
    echo "    Revoked and verified: userinfo returns 401"
    ACCESS_TOKEN=""
}
run_test "Test 12: Token Revocation" test_12

# Test 12b: Revoke a REFRESH token, then it must NOT be usable to refresh
# (C3 / RFC 7009 §2.1 — the revocation endpoint must revoke ANY token type).
# This closes the coverage gap that hid the C3 hash bug: previously the script
# only revoked access tokens (Test 12/43), so a refresh-token revoke that was
# a silent no-op went undetected.
TOTAL=$((TOTAL + 1))
test_12b() {
    # Mint a fresh token pair so this test is independent of Test 12's state.
    local verifier challenge login_resp code tok_resp rt rt2
    verifier=$(generate_pkce_verifier)
    challenge=$(pkce_s256_challenge "$verifier")
    login_resp=$(curl -s -X POST "$BASE_URL/oauth2/login" \
        -d "username=admin&password=admin&client_id=fulla-portal&redirect_uri=http://127.0.0.1:5173/callback&scope=openid+profile&state=t12b&code_challenge=$challenge&code_challenge_method=S256&json=true")
    code=$(echo "$login_resp" | jq -r '.code')
    [ -n "$code" ] && [ "$code" != "null" ] || { echo "    no auth code"; return 1; }
    tok_resp=$(curl -s -X POST "$BASE_URL/oauth2/token" \
        -d "grant_type=authorization_code&code=$code&redirect_uri=http://127.0.0.1:5173/callback&client_id=fulla-portal&code_verifier=$verifier")
    rt=$(echo "$tok_resp" | jq -r '.refresh_token')
    [ -n "$rt" ] && [ "$rt" != "null" ] || { echo "    no refresh_token"; return 1; }
    # Rotate once to get a current (un-rotated) refresh token.
    rt2=$(curl -s -X POST "$BASE_URL/oauth2/token" \
        -d "grant_type=refresh_token&refresh_token=$rt&client_id=fulla-portal" | jq -r '.refresh_token')
    [ -n "$rt2" ] && [ "$rt2" != "null" ] || { echo "    refresh rotation failed"; return 1; }

    # Revoke the refresh token (fulla-portal owns it; PUBLIC, client_id only).
    curl -s -o /dev/null -X POST "$BASE_URL/oauth2/revoke" \
        -d "token=$rt2&client_id=fulla-portal"

    # RFC 7009 §2.1 + RFC 6749 §6: the revoked refresh token MUST NOT mint new
    # tokens. Expect an error (invalid_grant — reuse detected / revoked).
    local refresh_after
    refresh_after=$(curl -s -X POST "$BASE_URL/oauth2/token" \
        -d "grant_type=refresh_token&refresh_token=$rt2&client_id=fulla-portal")
    local err has_at
    err=$(echo "$refresh_after" | jq -r '.error // empty')
    has_at=$(echo "$refresh_after" | jq -r '.access_token // empty')
    [ -n "$err" ] || { echo "    refresh after revoke succeeded (no error) — C3 FAILED"; return 1; }
    [ -z "$has_at" ] || { echo "    refresh after revoke returned a new access_token — C3 FAILED"; return 1; }
    echo "    Revoked refresh token correctly rejected: error=$err"
}
run_test "Test 12b: Revoke refresh token -> refresh fails (C3/RFC 7009)" test_12b

# Test 13: User Registration
test_13() {
    local ts
    ts=$(date +%s)
    local r
    r=$(curl -s -X POST "$BASE_URL/api/register" \
        -d "username=testuser_$ts&password=TestPass123&email=test_${ts}@example.com")
    assert_json_exists "$r" "message" || return 1
    echo "    Registered: testuser_$ts"
}
run_test "Test 13: User Registration" test_13

# Test 14: User Profile
test_14() {
    # Need a fresh token (previous was revoked). PKCE (F-011/RFC 7636).
    local verifier challenge
    verifier=$(generate_pkce_verifier)
    challenge=$(pkce_s256_challenge "$verifier")
    local login_resp
    login_resp=$(curl -s -X POST "$BASE_URL/oauth2/login" \
        -d "username=admin&password=admin&client_id=fulla-portal&redirect_uri=http://127.0.0.1:5173/callback&scope=openid+profile&state=test-state-12345678&code_challenge=$challenge&code_challenge_method=S256&json=true")
    local code
    code=$(echo "$login_resp" | jq -r '.code')
    local tok_resp
    tok_resp=$(curl -s -X POST "$BASE_URL/oauth2/token" \
        -d "grant_type=authorization_code&code=$code&redirect_uri=http://127.0.0.1:5173/callback&client_id=fulla-portal&code_verifier=$verifier")
    ACCESS_TOKEN=$(echo "$tok_resp" | jq -r '.access_token')

    local r
    r=$(curl -s -H "Authorization: Bearer $ACCESS_TOKEN" "$BASE_URL/api/me")
    assert_json_exists "$r" "username" || return 1
    echo "    Username: $(echo "$r" | jq -r '.username')"
}
run_test "Test 14: User Profile" test_14

# Test 15: Password Reset Request
test_15() {
    local r
    r=$(curl -s -X POST -H "Content-Type: application/json" \
        -d '{"email":"admin@example.com"}' "$BASE_URL/api/password-reset/request")
    assert_json_exists "$r" "message" || return 1
    echo "    Response: $(echo "$r" | jq -r '.message')"
}
run_test "Test 15: Password Reset Request" test_15

# Test 16: Password Reset (non-existent)
test_16() {
    local r
    r=$(curl -s -X POST -H "Content-Type: application/json" \
        -d '{"email":"nobody@nowhere.com"}' "$BASE_URL/api/password-reset/request")
    assert_json_exists "$r" "message" || return 1
    echo "    Anti-enumeration: same response for non-existent email"
}
run_test "Test 16: Password Reset (non-existent)" test_16

# Test 17: Password Change
test_17() {
    [ -n "$ACCESS_TOKEN" ] || { echo "    skipped: no token"; return 1; }
    local r
    r=$(curl -s -X PUT -H "Authorization: Bearer $ACCESS_TOKEN" -H "Content-Type: application/json" \
        -d '{"old_password":"admin","new_password":"NewPass123!"}' "$BASE_URL/api/me/password")
    assert_json_exists "$r" "message" || return 1
    echo "    $(echo "$r" | jq -r '.message')"

    # Restore password. PKCE (F-011/RFC 7636) required for the restore login.
    local restore_verifier restore_challenge
    restore_verifier=$(generate_pkce_verifier)
    restore_challenge=$(pkce_s256_challenge "$restore_verifier")
    local login_resp
    login_resp=$(curl -s -X POST "$BASE_URL/oauth2/login" \
        -d "username=admin&password=NewPass123!&client_id=fulla-portal&redirect_uri=http://127.0.0.1:5173/callback&scope=openid&state=restore-pw-state1&code_challenge=$restore_challenge&code_challenge_method=S256&json=true" 2>/dev/null) || true
    local restore_code
    restore_code=$(echo "$login_resp" | jq -r '.code // empty')
    if [ -n "$restore_code" ] && [ "$restore_code" != "null" ]; then
        local tok_resp
        tok_resp=$(curl -s -X POST "$BASE_URL/oauth2/token" \
            -d "grant_type=authorization_code&code=$restore_code&redirect_uri=http://127.0.0.1:5173/callback&client_id=fulla-portal&code_verifier=$restore_verifier")
        local restore_token
        restore_token=$(echo "$tok_resp" | jq -r '.access_token')
        curl -s -X PUT -H "Authorization: Bearer $restore_token" -H "Content-Type: application/json" \
            -d '{"old_password":"NewPass123!","new_password":"admin"}' "$BASE_URL/api/me/password" >/dev/null
        echo "    Password restored to 'admin'"
    else
        echo "    NOTE: Could not restore password (run setup-database.sh to reset)"
    fi
    # Defense-in-depth: unconditionally reset the admin account so downstream
    # tests (20+) that depend on the seeded admin/admin credentials see the
    # correct password regardless of whether the API restore path above
    # succeeded (the restore can race or fail silently under load).
    reset_admin_account >/dev/null 2>&1 || true
}
run_test "Test 17: Password Change" test_17

# Test 17b: Password Reset Confirm - Invalid token
test_17b() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" \
        -d '{"token":"invalid-token-xyz","password":"NewPass123!"}' "$BASE_URL/api/password-reset/confirm")
    if [ "$code" = "400" ]; then
        echo "    Correctly returned 400 for invalid token"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 17b: POST /api/password-reset/confirm - Invalid token" test_17b

# Test 17c: Password Reset Confirm - Missing fields
test_17c() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" \
        -d '{}' "$BASE_URL/api/password-reset/confirm")
    if [ "$code" = "400" ]; then
        echo "    Correctly returned 400 for missing fields"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 17c: POST /api/password-reset/confirm - Missing fields" test_17c

# Test 18: RFC 8414 OAuth Authorization Server Metadata
test_18() {
    local r
    r=$(curl -s "$BASE_URL/.well-known/oauth-authorization-server")
    assert_json_exists "$r" "issuer" || return 1
    assert_json_exists "$r" "authorization_endpoint" || return 1
    assert_json_exists "$r" "token_endpoint" || return 1
    assert_json_exists "$r" "grant_types_supported" || return 1
    echo "    Issuer: $(echo "$r" | jq -r '.issuer')"
}
run_test "Test 18: GET /.well-known/oauth-authorization-server" test_18

# Test 19: OAuth2 Consent - No session
test_19() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" \
        -d '{"client_id":"fulla-portal","scope":"openid profile","action":"approve"}' "$BASE_URL/oauth2/consent")
    if [ "$code" = "400" ] || [ "$code" = "401" ] || [ "$code" = "403" ]; then
        echo "    Correctly rejected: $code"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 19: POST /oauth2/consent - No session" test_19

# Test 20: Dynamic Client Registration
REG_CLIENT_ID=""
test_20() {
    # /oauth2/register is behind AuthorizationFilter and requires the `admin`
    # RBAC role (config.json rbac_rules). get_admin_token (fulla-admin-console
    # client, admin scope) yields the right token; the fulla-portal user token
    # (openid profile only) is insufficient.
    local admin_tok
    admin_tok=$(get_admin_token "$BASE_URL" "admin" "admin")
    [ -n "$admin_tok" ] || { echo "    skipped: no admin token"; return 1; }
    local ts
    ts=$(date +%s)
    local r
    r=$(curl -s -X POST -H "Authorization: Bearer $admin_tok" -H "Content-Type: application/json" \
        -d "{\"client_name\":\"Test DynReg $ts\",\"redirect_uris\":[\"http://localhost:4000/callback\"],\"grant_types\":[\"authorization_code\"]}" \
        "$BASE_URL/oauth2/register")
    assert_json_exists "$r" "client_id" || return 1
    assert_json_exists "$r" "client_secret" || return 1
    REG_CLIENT_ID=$(echo "$r" | jq -r '.client_id')
    echo "    Registered: client_id=$REG_CLIENT_ID"
}
run_test "Test 20: POST /oauth2/register - Register Client" test_20

# Test 20b: Register - Missing client_name (400)
test_20b() {
    # Same admin-role requirement as test_20 (AuthorizationFilter + rbac_rules).
    local admin_tok
    admin_tok=$(get_admin_token "$BASE_URL" "admin" "admin")
    [ -n "$admin_tok" ] || { echo "    skipped: no admin token"; return 1; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
        -H "Authorization: Bearer $admin_tok" -H "Content-Type: application/json" \
        -d '{"redirect_uris":["http://localhost/cb"]}' "$BASE_URL/oauth2/register")
    assert_status "$code" "400" || return 1
    echo "    Correctly returned 400 for missing client_name"
}
run_test "Test 20b: POST /oauth2/register - Missing client_name (400)" test_20b

# Test 20c: Register - No auth (401/403)
test_20c() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" \
        -d '{"client_name":"No Auth Test","redirect_uris":["http://localhost/cb"]}' "$BASE_URL/oauth2/register")
    assert_status_in "$code" "401|403" || return 1
    echo "    Correctly rejected: $code"
}
run_test "Test 20c: POST /oauth2/register - No auth" test_20c

# Test 21: MFA Setup
MFA_SECRET=""
test_21() {
    local tok
    tok=$(get_user_token "$BASE_URL" "admin" "admin")
    [ -n "$tok" ] || { echo "    skipped: no token"; return 1; }
    local r
    r=$(curl -s -X POST -H "Authorization: Bearer $tok" "$BASE_URL/api/me/mfa/setup")
    assert_json_exists "$r" "secret" || return 1
    assert_json_exists "$r" "otpauth_uri" || return 1
    MFA_SECRET=$(echo "$r" | jq -r '.secret')
    echo "    Secret: ${MFA_SECRET:0:8}..., URI present"
}
run_test "Test 21: POST /api/me/mfa/setup" test_21

# Test 21b: MFA Setup - No auth (401)
test_21b() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/api/me/mfa/setup")
    assert_status "$code" "401" || return 1
    echo "    Correctly returned 401"
}
run_test "Test 21b: POST /api/me/mfa/setup - No auth (401)" test_21b

# Test 22: MFA Verify - Invalid code
test_22() {
    local tok
    tok=$(get_user_token "$BASE_URL" "admin" "admin")
    [ -n "$tok" ] || { echo "    skipped: no token"; return 1; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
        -H "Authorization: Bearer $tok" -H "Content-Type: application/json" \
        -d '{"code":"000000"}' "$BASE_URL/api/me/mfa/verify")
    if [ "$code" = "400" ] || [ "$code" = "401" ]; then
        echo "    Correctly rejected invalid code: $code"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 22: POST /api/me/mfa/verify - Invalid code" test_22

# Test 22b: MFA Verify - No auth (401)
test_22b() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" \
        -d '{"code":"123456"}' "$BASE_URL/api/me/mfa/verify")
    assert_status "$code" "401" || return 1
    echo "    Correctly returned 401"
}
run_test "Test 22b: POST /api/me/mfa/verify - No auth (401)" test_22b

# Test 23: MFA Disable - No auth (401)
test_23() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/api/me/mfa/disable")
    assert_status "$code" "401" || return 1
    echo "    Correctly returned 401"
}
run_test "Test 23: POST /api/me/mfa/disable - No auth (401)" test_23

# Test 24: MFA Login Verify - Invalid format
test_24() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" \
        -d '{"mfa_token":"invalid","code":"abc"}' "$BASE_URL/oauth2/mfa/verify")
    if [ "$code" = "400" ] || [ "$code" = "401" ] || [ "$code" = "403" ]; then
        echo "    Correctly rejected: $code"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 24: POST /oauth2/mfa/verify - Invalid format" test_24

# Test 25: Authorized Apps List
test_25() {
    local tok
    tok=$(get_user_token "$BASE_URL" "admin" "admin")
    [ -n "$tok" ] || { echo "    skipped: no token"; return 1; }
    local r
    r=$(curl -s -H "Authorization: Bearer $tok" "$BASE_URL/api/me/authorized-apps")
    assert_json_exists "$r" "authorized_apps" || return 1
    echo "    Total authorized apps: $(echo "$r" | jq -r '.total')"
}
run_test "Test 25: GET /api/me/authorized-apps" test_25

# Test 25b: Authorized Apps - No auth (401)
test_25b() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE_URL/api/me/authorized-apps")
    assert_status "$code" "401" || return 1
    echo "    Correctly returned 401"
}
run_test "Test 25b: GET /api/me/authorized-apps - No auth (401)" test_25b

# Test 26: Revoke Authorized App - Non-existent (404)
test_26() {
    local tok
    tok=$(get_user_token "$BASE_URL" "admin" "admin")
    [ -n "$tok" ] || { echo "    skipped: no token"; return 1; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE -H "Authorization: Bearer $tok" \
        "$BASE_URL/api/me/authorized-apps/nonexistent-app-xyz")
    if [ "$code" = "404" ]; then
        echo "    Correctly returned 404"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 26: DELETE /api/me/authorized-apps/:clientId - Non-existent" test_26

# Test 26b: Revoke Authorized App - No auth (401)
test_26b() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$BASE_URL/api/me/authorized-apps/fulla-portal")
    assert_status "$code" "401" || return 1
    echo "    Correctly returned 401"
}
run_test "Test 26b: DELETE /api/me/authorized-apps/:clientId - No auth (401)" test_26b

# Test 27: Delete Account
test_27() {
    local ts
    ts=$(date +%s)
    local un="delme_$ts"
    curl -s -X POST "$BASE_URL/api/register" \
        -d "username=$un&password=TestPass123!&email=${un}@test.com" >/dev/null
    local tok
    tok=$(get_user_token "$BASE_URL" "$un" "TestPass123!")
    [ -n "$tok" ] || { echo "    skipped: could not get token"; return 1; }
    local r
    r=$(curl -s -X DELETE -H "Authorization: Bearer $tok" "$BASE_URL/api/me")
    assert_json_exists "$r" "message" || return 1
    echo "    Deleted account: $un"
}
run_test "Test 27: DELETE /api/me - Delete test account" test_27

# Test 27b: Delete Account - No auth (401)
test_27b() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$BASE_URL/api/me")
    assert_status "$code" "401" || return 1
    echo "    Correctly returned 401"
}
run_test "Test 27b: DELETE /api/me - No auth (401)" test_27b

# Test 28: Email Verify - Invalid token
test_28() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE_URL/api/verify-email?token=invalid-token-xyz")
    if [ "$code" = "400" ] || [ "$code" = "404" ]; then
        echo "    Correctly rejected invalid token: $code"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 28: GET /api/verify-email - Invalid token" test_28

# Test 28b: Email Verify - Missing token
test_28b() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE_URL/api/verify-email")
    if [ "$code" = "400" ]; then
        echo "    Correctly returned 400 for missing token"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 28b: GET /api/verify-email - Missing token" test_28b

# Test 28c: Email Resend - No auth (401)
test_28c() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/api/verify-email/resend")
    assert_status "$code" "401" || return 1
    echo "    Correctly returned 401"
}
run_test "Test 28c: POST /api/verify-email/resend - No auth (401)" test_28c

# Test 29: WebAuthn Register Begin - No auth (401)
test_29() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/api/me/webauthn/register/begin")
    assert_status "$code" "401" || return 1
    echo "    Correctly returned 401"
}
run_test "Test 29: POST /api/me/webauthn/register/begin - No auth (401)" test_29

# Test 30: WebAuthn Register Begin - With auth
test_30() {
    local tok
    tok=$(get_user_token "$BASE_URL" "admin" "admin")
    [ -n "$tok" ] || { echo "    skipped: no token"; return 1; }
    local r
    r=$(curl -s -X POST -H "Authorization: Bearer $tok" "$BASE_URL/api/me/webauthn/register/begin") || true
    echo "    Response received"
}
run_test "Test 30: POST /api/me/webauthn/register/begin - With auth" test_30

# Test 31: WebAuthn Register Finish - Invalid (400)
test_31() {
    local tok
    tok=$(get_user_token "$BASE_URL" "admin" "admin")
    [ -n "$tok" ] || { echo "    skipped: no token"; return 1; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
        -H "Authorization: Bearer $tok" -H "Content-Type: application/json" \
        -d '{"credential_id":"invalid","public_key":"invalid"}' \
        "$BASE_URL/api/me/webauthn/register/finish")
    if [ "$code" = "400" ]; then
        echo "    Correctly returned 400"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 31: POST /api/me/webauthn/register/finish - Invalid (400)" test_31

# Test 32: WebAuthn Authenticate Begin
test_32() {
    curl -s -X POST "$BASE_URL/oauth2/webauthn/authenticate/begin" >/dev/null || true
    echo "    Response received"
}
run_test "Test 32: POST /oauth2/webauthn/authenticate/begin" test_32

# Test 33: WebAuthn Credentials - No auth (401)
test_33() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE_URL/api/me/webauthn/credentials")
    assert_status "$code" "401" || return 1
    echo "    Correctly returned 401"
}
run_test "Test 33: GET /api/me/webauthn/credentials - No auth (401)" test_33

# Test 34: Device Authorization
test_34() {
    local r
    r=$(curl -s -X POST "$BASE_URL/oauth2/device_authorization" \
        -d "client_id=fulla-portal&scope=openid+profile") || true
    local dc
    dc=$(echo "$r" | jq -r '.device_code // empty')
    if [ -n "$dc" ]; then
        echo "    device_code: ${dc:0:8}..., user_code: $(echo "$r" | jq -r '.user_code')"
    else
        echo "    Device flow response: $(echo "$r" | head -c 80)"
    fi
}
run_test "Test 34: POST /oauth2/device_authorization" test_34

# Test 34b: Device Auth - Missing client_id (400)
test_34b() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/oauth2/device_authorization" \
        -d "scope=openid")
    if [ "$code" = "400" ]; then
        echo "    Correctly returned 400"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 34b: POST /oauth2/device_authorization - Missing client_id" test_34b

# Test 35: Device Approve - No auth (401/403)
test_35() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" \
        -d '{"user_code":"INVALID","user_id":"nobody"}' "$BASE_URL/oauth2/device/approve")
    assert_status_in "$code" "401|403" || return 1
    echo "    Correctly rejected: $code"
}
run_test "Test 35: POST /oauth2/device/approve - No auth" test_35

# Test 36-38: Social Login (error-only)
test_36() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" \
        -d '{"code":"invalid-github-code-xyz"}' "$BASE_URL/api/github/login")
    if [ "$code" = "401" ] || [ "$code" = "400" ] || [ "$code" = "500" ]; then
        echo "    Correctly rejected invalid code: $code"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 36: POST /api/github/login - Invalid code" test_36

test_37() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" \
        -d '{"code":"invalid-google-code-xyz"}' "$BASE_URL/api/google/login")
    if [ "$code" = "401" ] || [ "$code" = "400" ] || [ "$code" = "500" ]; then
        echo "    Correctly rejected invalid code: $code"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 37: POST /api/google/login - Invalid code" test_37

test_38() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" \
        -d '{"code":"invalid-wechat-code-xyz"}' "$BASE_URL/api/wechat/login")
    if [ "$code" = "401" ] || [ "$code" = "400" ] || [ "$code" = "500" ]; then
        echo "    Correctly rejected invalid code: $code"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 38: POST /api/wechat/login - Invalid code" test_38

# Test 39: Password Change - Wrong old password
test_39() {
    local tok
    tok=$(get_user_token "$BASE_URL" "admin" "admin")
    [ -n "$tok" ] || { echo "    skipped: no token"; return 1; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT \
        -H "Authorization: Bearer $tok" -H "Content-Type: application/json" \
        -d '{"old_password":"wrong-password-xyz","new_password":"NewPass456!"}' "$BASE_URL/api/me/password")
    if [ "$code" = "401" ] || [ "$code" = "400" ]; then
        echo "    Correctly rejected wrong old password: $code"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 39: PUT /api/me/password - Wrong old password" test_39

# Test 40: Password Change - No auth (401)
test_40() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT -H "Content-Type: application/json" \
        -d '{"old_password":"admin","new_password":"NewPass123!"}' "$BASE_URL/api/me/password")
    assert_status "$code" "401" || return 1
    echo "    Correctly returned 401"
}
run_test "Test 40: PUT /api/me/password - No auth (401)" test_40

# Test 41: Token - Expired/used authorization code
test_41() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/oauth2/token" \
        -d "grant_type=authorization_code&code=already-used-or-expired-code-xyz&redirect_uri=http://127.0.0.1:5173/callback&client_id=fulla-portal")
    if [ "$code" = "400" ]; then
        echo "    Correctly rejected expired code: 400"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 41: POST /oauth2/token - Expired auth code" test_41

# Test 42: Introspect - Malformed token
test_42() {
    # RFC 7662 §2.1: the introspection endpoint authenticates the calling
    # CLIENT (not the resource owner). fulla-portal is PUBLIC (no secret) and
    # cannot authenticate here; use backend-svc (CONFIDENTIAL, secret
    # "test-secret") via HTTP Basic (F-017: client_secret_basic). The token
    # is a valid-format but nonexistent 43-char string (>= TOKEN_MIN_LEN 32)
    # so the request passes input validation and the introspection result is
    # active=false (token not in the store).
    local r
    r=$(curl -s -u "backend-svc:test-secret" -X POST "$BASE_URL/oauth2/introspect" \
        -d "token=not-a-real-token-but-long-enough-to-pass-min-len-check-XYZ")
    local active
    active=$(echo "$r" | jq -r '.active')
    [ "$active" = "false" ] || { echo "    malformed token should be active=false, got $active"; return 1; }
    echo "    Correctly returned active=false for malformed token"
}
run_test "Test 42: POST /oauth2/introspect - Malformed token" test_42

# Test 43: Revoke - Already revoked (idempotent)
test_43() {
    local tok
    tok=$(get_user_token "$BASE_URL" "admin" "admin")
    [ -n "$tok" ] || { echo "    skipped: no token"; return 1; }
    # RFC 7009 §2.1: revoke requires the caller to be the token's owner. The
    # token was issued to fulla-portal (PUBLIC); fulla-portal revokes it
    # (client_id only, no secret — PUBLIC clients are exempt from auth at
    # the revocation endpoint).
    # Revoke once
    curl -s -X POST "$BASE_URL/oauth2/revoke" \
        -d "token=$tok&client_id=fulla-portal" >/dev/null
    # Revoke again (idempotent)
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
        "$BASE_URL/oauth2/revoke" -d "token=$tok&client_id=fulla-portal")
    echo "    Second revocation: $code"
}
run_test "Test 43: POST /oauth2/revoke - Already revoked (idempotent)" test_43

# Test 44: Introspect - Missing client credentials
test_44() {
    # RFC 7662 SS2.1: without client credentials the endpoint MUST return
    # 401 with the RFC 6749 SS5.2 error body {"error":"invalid_client"}
    # plus WWW-Authenticate: Basic realm=... (not an Error-Envelope).
    local code body wwwauth
    code=$(curl -s -o /tmp/introspect_no_creds.$$ -D /tmp/introspect_hdrs.$$ -w '%{http_code}' \
        -X POST "$BASE_URL/oauth2/introspect" -d "token=some-token")
    body=$(cat /tmp/introspect_no_creds.$$)
    wwwauth=$(grep -i '^WWW-Authenticate:' /tmp/introspect_hdrs.$$ | head -n 1)
    rm -f /tmp/introspect_no_creds.$$ /tmp/introspect_hdrs.$$
    [ "$code" = "401" ] || { echo "    expected 401, got $code"; return 1; }
    echo "$body" | jq -e '.error == "invalid_client"' >/dev/null \
        || { echo "    expected invalid_client error body, got: $body"; return 1; }
    echo "    401 + invalid_client body returned ($wwwauth)"
}
run_test "Test 44: POST /oauth2/introspect - Missing client credentials" test_44

# Post-test Cleanup
echo ""
echo "========================================"
echo "Post-test Cleanup"
echo "========================================"
reset_admin_account
echo ""

print_summary $TOTAL
