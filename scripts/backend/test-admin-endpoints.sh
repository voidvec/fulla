#!/usr/bin/env bash
# test-admin-endpoints.sh - Admin API endpoint tests (Linux/macOS)
# Equivalent of test-admin-endpoints.ps1
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common-test-functions.sh"

BASE_URL="${1:-http://127.0.0.1:5555}"
ACCESS_TOKEN=""

TOTAL=52

echo "========================================"
echo "Pre-test Setup"
echo "========================================"
reset_admin_account
echo ""

echo "========================================"
echo "Admin API Endpoints Tests ($TOTAL tests)"
echo "========================================"
echo "Base URL: $BASE_URL"
echo ""

# Helper: get auth headers for curl
auth_header() {
    echo "Authorization: Bearer $ACCESS_TOKEN"
}

# Setup: Admin Login + Token
# F-011/RFC 7636 (RFC 9700 §2.1.1): PKCE mandatory for PUBLIC clients.
# fulla-admin-console is PUBLIC → login carries code_challenge, token exchange
# carries the matching code_verifier. client_secret is empty (PUBLIC client,
# token_endpoint_auth_method='none' — F-017 rejects any secret).
test_setup_login() {
    local verifier challenge
    verifier=$(generate_pkce_verifier)
    challenge=$(pkce_s256_challenge "$verifier")
    local login_resp
    login_resp=$(curl -s -X POST "$BASE_URL/oauth2/login" \
        -d "username=admin&password=admin&client_id=fulla-admin-console&redirect_uri=http://localhost:5174/admin/callback&scope=openid+profile+admin&state=admin-test-state&code_challenge=$challenge&code_challenge_method=S256&json=true")
    local code
    code=$(echo "$login_resp" | jq -r '.code')
    [ -n "$code" ] && [ "$code" != "null" ] || { echo "    no auth code from login"; return 1; }

    local tok_resp
    tok_resp=$(curl -s -X POST "$BASE_URL/oauth2/token" \
        -d "grant_type=authorization_code&code=$code&redirect_uri=http://localhost:5174/admin/callback&client_id=fulla-admin-console&client_secret=&code_verifier=$verifier")
    ACCESS_TOKEN=$(echo "$tok_resp" | jq -r '.access_token')
    [ -n "$ACCESS_TOKEN" ] && [ "$ACCESS_TOKEN" != "null" ] || { echo "    no access_token"; return 1; }
    echo "    Token: ${ACCESS_TOKEN:0:16}..."
}
run_test "Setup: Admin Login + Token" test_setup_login

# Test 1: Dashboard Stats
test_1() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/dashboard/stats")
    assert_json_field "$r" "status" "success" || return 1
    assert_json_exists "$r" "total_users" || return 1
    assert_json_exists "$r" "total_clients" || return 1
}
run_test "Test 1: GET /api/admin/dashboard/stats" test_1

# Test 2: Client Detail
test_2() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/clients/fulla-portal")
    assert_json_field "$r" "status" "success" || return 1
    assert_json_field "$r" "client_id" "fulla-portal" || return 1
}
run_test "Test 2: GET /api/admin/clients/:id - Client Detail" test_2

# Test 3: Client Not Found
test_3() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -H "$(auth_header)" "$BASE_URL/api/admin/clients/nonexistent-xyz")
    assert_status "$code" "404" || return 1
    echo "    Correctly returned 404"
}
run_test "Test 3: GET /api/admin/clients/:id - Not Found (404)" test_3

# Test 4: Update Client
test_4() {
    local r
    r=$(curl -s -X PUT -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"name":"Vue Frontend Updated"}' "$BASE_URL/api/admin/clients/fulla-portal")
    assert_json_field "$r" "status" "success" || return 1
    # Restore
    curl -s -X PUT -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"name":"Vue Frontend"}' "$BASE_URL/api/admin/clients/fulla-portal" >/dev/null
}
run_test "Test 4: PUT /api/admin/clients/:id - Update Client" test_4

# Test 5: Client Scopes
test_5() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/clients/fulla-portal/scopes")
    assert_json_field "$r" "status" "success" || return 1
    assert_json_exists "$r" "scopes" || return 1
}
run_test "Test 5: GET /api/admin/clients/:id/scopes" test_5

# Test 6: Update Client Scopes
test_6() {
    local r
    r=$(curl -s -X PUT -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"scopes":["openid","profile","email"]}' "$BASE_URL/api/admin/clients/fulla-portal/scopes")
    assert_json_field "$r" "status" "success" || return 1
}
run_test "Test 6: PUT /api/admin/clients/:id/scopes - Update" test_6

# Test 6b: Client List
test_6b() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/clients")
    assert_json_field "$r" "status" "success" || return 1
    local count
    count=$(echo "$r" | jq '.clients | length')
    [ "$count" -ge 2 ] || { echo "    expected >= 2 clients, got $count"; return 1; }
    echo "    total=$(echo "$r" | jq -r '.total'), count=$count"
}
run_test "Test 6b: GET /api/admin/clients - List All" test_6b

# Test 6c: Create Client
NEW_CLIENT_ID=""
NEW_CLIENT_SECRET=""
test_6c() {
    local ts
    ts=$(date +%s)
    local r
    r=$(curl -s -X POST -H "$(auth_header)" -H "Content-Type: application/json" \
        -d "{\"name\":\"Test Client $ts\",\"redirect_uris\":\"http://localhost:3000/callback\",\"allowed_grant_types\":\"authorization_code\",\"client_type\":\"CONFIDENTIAL\"}" \
        "$BASE_URL/api/admin/clients")
    assert_json_field "$r" "status" "success" || return 1
    NEW_CLIENT_ID=$(echo "$r" | jq -r '.client_id')
    NEW_CLIENT_SECRET=$(echo "$r" | jq -r '.client_secret')
    [ -n "$NEW_CLIENT_ID" ] && [ "$NEW_CLIENT_ID" != "null" ] || { echo "    missing client_id"; return 1; }
    echo "    Created: client_id=$NEW_CLIENT_ID"
}
run_test "Test 6c: POST /api/admin/clients - Create Client" test_6c

# Test 6d: Create Client with missing name (400)
test_6d() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "$(auth_header)" \
        -H "Content-Type: application/json" -d '{"redirect_uris":"http://localhost/cb"}' \
        "$BASE_URL/api/admin/clients")
    assert_status "$code" "400" || return 1
    echo "    Correctly returned 400 for missing name"
}
run_test "Test 6d: POST /api/admin/clients - Missing name (400)" test_6d

# Test 6e: Reset Client Secret
NEW_CLIENT_SECRET2=""
test_6e() {
    [ -n "$NEW_CLIENT_ID" ] || { echo "    skipped: no test client"; return 1; }
    local r
    r=$(curl -s -X POST -H "$(auth_header)" "$BASE_URL/api/admin/clients/$NEW_CLIENT_ID/reset-secret")
    assert_json_field "$r" "status" "success" || return 1
    NEW_CLIENT_SECRET2=$(echo "$r" | jq -r '.client_secret')
    [ "$NEW_CLIENT_SECRET2" != "$NEW_CLIENT_SECRET" ] || { echo "    secret not changed"; return 1; }
    echo "    New secret differs from original: confirmed"
}
run_test "Test 6e: POST /api/admin/clients/:id/reset-secret" test_6e

# Test 6f: Reset Secret - Non-existent (404)
test_6f() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "$(auth_header)" \
        "$BASE_URL/api/admin/clients/nonexistent-xyz/reset-secret")
    assert_status "$code" "404" || return 1
    echo "    Correctly returned 404"
}
run_test "Test 6f: POST /api/admin/clients/:id/reset-secret - Not Found (404)" test_6f

# Test 6g: Delete Client
test_6g() {
    [ -n "$NEW_CLIENT_ID" ] || { echo "    skipped: no test client"; return 1; }
    local r
    r=$(curl -s -X DELETE -H "$(auth_header)" "$BASE_URL/api/admin/clients/$NEW_CLIENT_ID")
    assert_json_field "$r" "status" "success" || return 1
    # Verify gone
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -H "$(auth_header)" "$BASE_URL/api/admin/clients/$NEW_CLIENT_ID")
    [ "$code" = "404" ] || { echo "    client should be deleted but returned $code"; return 1; }
    echo "    Deleted and verified gone"
    NEW_CLIENT_ID=""
}
run_test "Test 6g: DELETE /api/admin/clients/:id - Delete" test_6g

# Test 6h: Delete Non-existent Client (404)
test_6h() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE -H "$(auth_header)" \
        "$BASE_URL/api/admin/clients/nonexistent-delete-xyz")
    assert_status "$code" "404" || return 1
    echo "    Correctly returned 404"
}
run_test "Test 6h: DELETE /api/admin/clients/:id - Not Found (404)" test_6h

# Test 7: Token List
test_7() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/tokens?page=1&per_page=10")
    assert_json_exists "$r" "tokens" || return 1
    assert_json_exists "$r" "total" || return 1
}
run_test "Test 7: GET /api/admin/tokens - Token List" test_7

# Test 8: Tokens Filter
test_8() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/tokens?client_id=fulla-admin-console&page=1&per_page=50")
    assert_json_exists "$r" "tokens" || return 1
}
run_test "Test 8: GET /api/admin/tokens - Filter by client_id" test_8

# Test 9: Revoke by client
test_9() {
    local r
    r=$(curl -s -X POST -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"client_id":"backend-svc"}' "$BASE_URL/api/admin/tokens/revoke-by-client")
    assert_json_field "$r" "status" "success" || return 1
}
run_test "Test 9: POST /api/admin/tokens/revoke-by-client" test_9

# Test 10: Revoke by user
test_10() {
    local r
    r=$(curl -s -X POST -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"user_id":"nonexistent-user-12345"}' "$BASE_URL/api/admin/tokens/revoke-by-user")
    assert_json_field "$r" "status" "success" || return 1
}
run_test "Test 10: POST /api/admin/tokens/revoke-by-user" test_10

# Test 11: OIDC Keys
test_11() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/oidc/keys")
    assert_json_field "$r" "status" "success" || return 1
    # #110-B: live keystore view. Rotation-proof assertions: exactly one
    # entry is "active" and it carries active_kid -- keys[0] is merely the
    # filename-sorted first key and may be a "published" (retiring) one.
    assert_json_field "$r" "keys[0].kty" "RSA" || return 1
    assert_json_field "$r" "keys[0].alg" "RS256" || return 1
    local active_kid active_count
    active_kid=$(echo "$r" | jq -r '.active_kid')
    active_count=$(echo "$r" | jq -r '[.keys[] | select(.status == "active")] | length')
    if [ "$active_count" != "1" ]; then
        echo -e "    ${C_RED}[FAIL] expected exactly 1 active key, got $active_count${C_NC}"
        return 1
    fi
    assert_json_field "$r" "keys[] | select(.status == \"active\") | .kid" "$active_kid" || return 1
}
run_test "Test 11: GET /api/admin/oidc/keys" test_11

# Test 11b: Single Token Revoke
test_11b() {
    # Issue a dedicated throwaway token to revoke. Never pick tokens[0] from the
    # list here: it is ordered by issued_at DESC, so the first row IS this
    # script's own live admin session -- revoking its prefix cascades 401s
    # through every remaining test. PKCE (F-011/RFC 7636) required for the
    # fulla-admin-console PUBLIC client.
    local login_resp code tok_resp throwaway verifier challenge
    verifier=$(generate_pkce_verifier)
    challenge=$(pkce_s256_challenge "$verifier")
    login_resp=$(curl -s -X POST "$BASE_URL/oauth2/login" \
        -d "username=admin&password=admin&client_id=fulla-admin-console&redirect_uri=http://localhost:5174/admin/callback&scope=openid+profile+admin&state=admin-test-11b&code_challenge=$challenge&code_challenge_method=S256&json=true")
    code=$(echo "$login_resp" | jq -r '.code')
    [ -n "$code" ] && [ "$code" != "null" ] || { echo "    no auth code for throwaway token"; return 1; }
    tok_resp=$(curl -s -X POST "$BASE_URL/oauth2/token" \
        -d "grant_type=authorization_code&code=$code&redirect_uri=http://localhost:5174/admin/callback&client_id=fulla-admin-console&client_secret=&code_verifier=$verifier")
    throwaway=$(echo "$tok_resp" | jq -r '.access_token')
    [ -n "$throwaway" ] && [ "$throwaway" != "null" ] || { echo "    no throwaway access_token"; return 1; }

    # Server stores SHA-256(raw) uppercase hex (CryptoUtils::hashToken);
    # token_prefix is its first 8 chars, so we can target the throwaway
    # token deterministically without consulting the list.
    local hash prefix
    if command -v sha256sum >/dev/null 2>&1; then
        hash=$(printf '%s' "$throwaway" | sha256sum | cut -d' ' -f1)
    else
        hash=$(printf '%s' "$throwaway" | shasum -a 256 | cut -d' ' -f1)
    fi
    prefix=$(printf '%s' "$hash" | cut -c1-8 | tr '[:lower:]' '[:upper:]')
    local r
    r=$(curl -s -X DELETE -H "$(auth_header)" "$BASE_URL/api/admin/tokens/$prefix")
    assert_json_field "$r" "status" "success" || return 1
    echo "    Revoked throwaway token prefix: $prefix"
}
run_test "Test 11b: DELETE /api/admin/tokens/:tokenPrefix - Single Revoke" test_11b

# Test 12: User List
ADMIN_USER_ID=""
test_12() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/users?q=admin")
    assert_json_field "$r" "status" "success" || return 1
    ADMIN_USER_ID=$(echo "$r" | jq -r '.users[] | select(.username=="admin") | .id')
    [ -n "$ADMIN_USER_ID" ] && [ "$ADMIN_USER_ID" != "null" ] || { echo "    admin user not found"; return 1; }
    echo "    admin id=$ADMIN_USER_ID"
}
run_test "Test 12: GET /api/admin/users - List" test_12

# Test 13: User Detail
test_13() {
    [ -n "$ADMIN_USER_ID" ] || { echo "    skipped: no user id"; return 1; }
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/users/$ADMIN_USER_ID")
    assert_json_field "$r" "status" "success" || return 1
    assert_json_field "$r" "username" "admin" || return 1
}
run_test "Test 13: GET /api/admin/users/:id - Detail" test_13

# Test 14: User Not Found
test_14() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -H "$(auth_header)" "$BASE_URL/api/admin/users/99999999")
    assert_status "$code" "404" || return 1
    echo "    Correctly returned 404"
}
run_test "Test 14: GET /api/admin/users/:id - Not Found" test_14

# Test 15: Update User
test_15() {
    [ -n "$ADMIN_USER_ID" ] || { echo "    skipped"; return 1; }
    local r
    r=$(curl -s -X PUT -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"email_verified":true}' "$BASE_URL/api/admin/users/$ADMIN_USER_ID")
    assert_json_field "$r" "status" "success" || return 1
}
run_test "Test 15: PUT /api/admin/users/:id - Update" test_15

# Test 16: User Roles
test_16() {
    [ -n "$ADMIN_USER_ID" ] || { echo "    skipped"; return 1; }
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/users/$ADMIN_USER_ID/roles")
    assert_json_field "$r" "status" "success" || return 1
}
run_test "Test 16: GET /api/admin/users/:id/roles" test_16

# Test 17: Disable/Enable User
TEST_USER_ID=""
test_17() {
    local ts
    ts=$(date +%s)
    # Create test user
    curl -s -X POST "$BASE_URL/api/register" \
        -d "username=testuser_$ts&password=TestPass123&email=t_${ts}@test.com" >/dev/null
    local users
    users=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/users?q=testuser_$ts")
    TEST_USER_ID=$(echo "$users" | jq -r ".users[] | select(.username==\"testuser_$ts\") | .id")
    [ -n "$TEST_USER_ID" ] && [ "$TEST_USER_ID" != "null" ] || { echo "    test user not found"; return 1; }
    # Disable
    local r
    r=$(curl -s -X PUT -H "$(auth_header)" "$BASE_URL/api/admin/users/$TEST_USER_ID/disable")
    assert_json_field "$r" "status" "success" || return 1
    # Enable
    r=$(curl -s -X POST -H "$(auth_header)" "$BASE_URL/api/admin/users/$TEST_USER_ID/enable")
    assert_json_field "$r" "status" "success" || return 1
    echo "    Disable/Enable cycle verified"
}
run_test "Test 17: Disable/Enable User" test_17

# Test 18: Role List
test_18() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/roles")
    assert_json_field "$r" "status" "success" || return 1
    local admin_role
    admin_role=$(echo "$r" | jq -r '.roles[] | select(.name=="admin") | .name')
    [ "$admin_role" = "admin" ] || { echo "    admin role not found"; return 1; }
}
run_test "Test 18: GET /api/admin/roles - List" test_18

# Test 19: Create Role
TEST_ROLE_ID=""
test_19() {
    local ts
    ts=$(date +%s)
    local r
    r=$(curl -s -X POST -H "$(auth_header)" -H "Content-Type: application/json" \
        -d "{\"name\":\"testrole_$ts\",\"description\":\"Test role\"}" "$BASE_URL/api/admin/roles")
    assert_json_field "$r" "status" "success" || return 1
    TEST_ROLE_ID=$(echo "$r" | jq -r '.id')
    echo "    Created: id=$TEST_ROLE_ID"
}
run_test "Test 19: POST /api/admin/roles - Create" test_19

# Test 20: Duplicate Role (409)
test_20() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "$(auth_header)" \
        -H "Content-Type: application/json" -d '{"name":"admin"}' "$BASE_URL/api/admin/roles")
    assert_status "$code" "409" || return 1
    echo "    Correctly returned 409"
}
run_test "Test 20: POST /api/admin/roles - Duplicate (409)" test_20

# Test 21: Update Role
test_21() {
    [ -n "$TEST_ROLE_ID" ] || { echo "    skipped"; return 1; }
    local r
    r=$(curl -s -X PUT -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"description":"Updated"}' "$BASE_URL/api/admin/roles/$TEST_ROLE_ID")
    assert_json_field "$r" "status" "success" || return 1
}
run_test "Test 21: PUT /api/admin/roles/:id - Update" test_21

# Test 22: Delete Role
test_22() {
    [ -n "$TEST_ROLE_ID" ] || { echo "    skipped"; return 1; }
    local r
    r=$(curl -s -X DELETE -H "$(auth_header)" "$BASE_URL/api/admin/roles/$TEST_ROLE_ID")
    assert_json_field "$r" "status" "success" || return 1
}
run_test "Test 22: DELETE /api/admin/roles/:id - Delete" test_22

# Test 23: Cannot delete built-in role
test_23() {
    local roles
    roles=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/roles")
    local admin_role_id
    admin_role_id=$(echo "$roles" | jq -r '.roles[] | select(.name=="admin") | .id')
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE -H "$(auth_header)" "$BASE_URL/api/admin/roles/$admin_role_id")
    assert_status "$code" "404" || return 1
    echo "    Correctly prevented deletion of built-in role"
}
run_test "Test 23: DELETE /api/admin/roles - Cannot delete built-in" test_23

# Test 24: Create Scope
TEST_SCOPE_ID=""
test_24() {
    local ts
    ts=$(date +%s)
    local r
    r=$(curl -s -X POST -H "$(auth_header)" -H "Content-Type: application/json" \
        -d "{\"name\":\"testscope_$ts\",\"description\":\"Test scope\",\"mapped_role\":\"user\",\"is_default\":false,\"requires_admin_role\":false}" \
        "$BASE_URL/api/admin/scopes")
    assert_json_field "$r" "status" "success" || return 1
    TEST_SCOPE_ID=$(echo "$r" | jq -r '.id')
    echo "    Created: id=$TEST_SCOPE_ID"
}
run_test "Test 24: POST /api/admin/scopes - Create" test_24

# Test 25: List Scopes
test_25() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/scopes")
    assert_json_field "$r" "status" "success" || return 1
    local openid
    openid=$(echo "$r" | jq -r '.scopes[] | select(.name=="openid") | .name')
    [ "$openid" = "openid" ] || { echo "    openid scope not found"; return 1; }
}
run_test "Test 25: GET /api/admin/scopes - List" test_25

# Test 26: Update Scope
test_26() {
    [ -n "$TEST_SCOPE_ID" ] || { echo "    skipped"; return 1; }
    local r
    r=$(curl -s -X PUT -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"description":"Updated","is_default":true}' "$BASE_URL/api/admin/scopes/$TEST_SCOPE_ID")
    assert_json_field "$r" "status" "success" || return 1
}
run_test "Test 26: PUT /api/admin/scopes/:id - Update" test_26

# Test 27: Delete Scope
test_27() {
    [ -n "$TEST_SCOPE_ID" ] || { echo "    skipped"; return 1; }
    local r
    r=$(curl -s -X DELETE -H "$(auth_header)" "$BASE_URL/api/admin/scopes/$TEST_SCOPE_ID")
    assert_json_field "$r" "status" "success" || return 1
}
run_test "Test 27: DELETE /api/admin/scopes/:id - Delete" test_27

# Test 28: Cannot delete built-in scope
test_28() {
    local scopes
    scopes=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/scopes")
    local openid_id
    openid_id=$(echo "$scopes" | jq -r '.scopes[] | select(.name=="openid") | .id')
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE -H "$(auth_header)" "$BASE_URL/api/admin/scopes/$openid_id")
    assert_status "$code" "404" || return 1
    echo "    Correctly prevented deletion of built-in scope"
}
run_test "Test 28: DELETE /api/admin/scopes - Cannot delete built-in" test_28

# Test 29: Duplicate Scope (409)
test_29() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "$(auth_header)" \
        -H "Content-Type: application/json" -d '{"name":"openid"}' "$BASE_URL/api/admin/scopes")
    assert_status "$code" "409" || return 1
    echo "    Correctly returned 409"
}
run_test "Test 29: POST /api/admin/scopes - Duplicate (409)" test_29

# Test 30: Unauthorized Access
test_30() {
    local endpoints=(
        "$BASE_URL/api/admin/clients/fulla-portal"
        "$BASE_URL/api/admin/tokens"
        "$BASE_URL/api/admin/roles"
        "$BASE_URL/api/admin/users/1"
        "$BASE_URL/api/admin/dashboard/stats"
    )
    local all_blocked=true
    for ep in "${endpoints[@]}"; do
        local code
        code=$(curl -s -o /dev/null -w '%{http_code}' "$ep")
        if [ "$code" != "401" ] && [ "$code" != "403" ]; then
            echo "    SECURITY: $ep returned $code without auth!"
            all_blocked=false
        fi
    done
    [ "$all_blocked" = "true" ] || { echo "    Some endpoints accessible without auth!"; return 1; }
    echo "    All 5 endpoints correctly require authentication"
}
run_test "Test 30: Unauthorized Access - Endpoints require auth" test_30

# Test 31: Assign Roles
test_31() {
    [ -n "$TEST_USER_ID" ] || { echo "    skipped"; return 1; }
    local r
    r=$(curl -s -X PUT -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"roles":["admin","user"]}' "$BASE_URL/api/admin/users/$TEST_USER_ID/roles")
    assert_json_field "$r" "status" "success" || return 1
}
run_test "Test 31: PUT /api/admin/users/:id/roles - Assign Roles" test_31

# Test 32: Audit Logs
test_32() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/logs?page=1&per_page=10")
    assert_json_field "$r" "status" "success" || return 1
    assert_json_exists "$r" "logs" || return 1
}
run_test "Test 32: GET /api/admin/logs - Audit Logs" test_32

# Test 33: Organization List
test_33() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/organizations")
    assert_json_exists "$r" "organizations" || return 1
}
run_test "Test 33: GET /api/admin/organizations - List" test_33

# Test 34: Create Organization
TEST_ORG_SLUG=""
test_34() {
    local ts
    ts=$(date +%s)
    local r
    r=$(curl -s -X POST -H "$(auth_header)" -H "Content-Type: application/json" \
        -d "{\"slug\":\"test-org-$ts\",\"name\":\"Test Organization $ts\"}" "$BASE_URL/api/admin/organizations")
    TEST_ORG_SLUG=$(echo "$r" | jq -r '.slug')
    [ -n "$TEST_ORG_SLUG" ] && [ "$TEST_ORG_SLUG" != "null" ] || { echo "    missing slug"; return 1; }
    echo "    Created: slug=$TEST_ORG_SLUG"
}
run_test "Test 34: POST /api/admin/organizations - Create" test_34

# Test 35: Organization Detail
test_35() {
    [ -n "$TEST_ORG_SLUG" ] || { echo "    skipped"; return 1; }
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/organizations/$TEST_ORG_SLUG")
    local slug
    slug=$(echo "$r" | jq -r '.slug')
    [ "$slug" = "$TEST_ORG_SLUG" ] || { echo "    slug mismatch"; return 1; }
}
run_test "Test 35: GET /api/admin/organizations/:slug - Detail" test_35

# Test 36: Organization Not Found
test_36() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -H "$(auth_header)" "$BASE_URL/api/admin/organizations/nonexistent-org-xyz")
    assert_status "$code" "404" || return 1
    echo "    Correctly returned 404"
}
run_test "Test 36: GET /api/admin/organizations/:slug - Not Found" test_36

# Test 37: Invalid slug (400)
test_37() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "$(auth_header)" \
        -H "Content-Type: application/json" -d '{"slug":"AB","name":"Bad"}' "$BASE_URL/api/admin/organizations")
    assert_status "$code" "400" || return 1
    echo "    Correctly returned 400 for invalid slug"
}
run_test "Test 37: POST /api/admin/organizations - Invalid slug (400)" test_37

# Test 38: Role with empty name (400)
test_38() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "$(auth_header)" \
        -H "Content-Type: application/json" -d '{"name":"","description":"No name"}' \
        "$BASE_URL/api/admin/roles")
    assert_status "$code" "400" || return 1
    echo "    Correctly returned 400 for empty name"
}
run_test "Test 38: POST /api/admin/roles - Empty name (400)" test_38

# Test 39: Client update with empty body (no updatable fields → 400)
test_39() {
    # The server validates that at least one updatable field (name,
    # redirect_uris, allowed_grant_types) is present in the body. An empty
    # body {} is a "no fields to update" client error (400), not a silent
    # no-op success. This matches the updateClient validation gate
    # (ClientManagementService.cc) and is stricter-but-correct.
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{}' "$BASE_URL/api/admin/clients/fulla-portal")
    assert_status "$code" "400" || return 1
    echo "    Empty body update correctly rejected with 400"
}
run_test "Test 39: PUT /api/admin/clients/:id - Empty body" test_39

# Test 40: Large per_page tokens
test_40() {
    local r
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/tokens?page=1&per_page=1000")
    assert_json_exists "$r" "tokens" || return 1
    echo "    Returned $(echo "$r" | jq '.tokens | length') tokens"
}
run_test "Test 40: GET /api/admin/tokens - Large per_page" test_40

# Test 41: Revoke by non-existent client
test_41() {
    local r
    r=$(curl -s -X POST -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"client_id":"nonexistent-client-xyz"}' "$BASE_URL/api/admin/tokens/revoke-by-client")
    assert_json_field "$r" "status" "success" || return 1
    local count
    count=$(echo "$r" | jq -r '.count')
    [ "$count" = "0" ] || { echo "    expected 0 revoked, got $count"; return 1; }
    echo "    Revoked $count tokens (expected 0)"
}
run_test "Test 41: POST /api/admin/tokens/revoke-by-client - Non-existent" test_41

# Test 42: Unauthorized Access - New endpoints
test_42() {
    # Each endpoint is probed with its correct HTTP method. The auth gate
    # runs before the method/handler, so a missing-token request must return
    # 401 (or 403) regardless of whether the method is otherwise valid. POST
    # endpoints get a JSON body; GET endpoints get no body.
    local post_endpoints=(
        "$BASE_URL/api/admin/clients"
        "$BASE_URL/api/me/mfa/setup"
    )
    local get_endpoints=(
        "$BASE_URL/api/me/authorized-apps"
        "$BASE_URL/api/me/webauthn/credentials"
    )
    local all_blocked=true
    for ep in "${post_endpoints[@]}"; do
        local code
        code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" -d '{}' "$ep" 2>/dev/null || true)
        if [ "$code" != "401" ] && [ "$code" != "403" ]; then
            echo "    WARNING: POST $ep returned $code"
            all_blocked=false
        fi
    done
    for ep in "${get_endpoints[@]}"; do
        local code
        code=$(curl -s -o /dev/null -w '%{http_code}' "$ep" 2>/dev/null || true)
        if [ "$code" != "401" ] && [ "$code" != "403" ]; then
            echo "    WARNING: GET $ep returned $code"
            all_blocked=false
        fi
    done
    [ "$all_blocked" = "true" ] || { echo "    Some endpoints accessible without auth!"; return 1; }
    echo "    All 4 new endpoints correctly require authentication"
}
run_test "Test 42: Unauthorized Access - New endpoints" test_42

# Test 43: Non-admin denied admin dashboard stats
test_43() {
    local ts
    ts=$(date +%s)
    local un="nonadmin_$ts"
    curl -s -X POST "$BASE_URL/api/register" \
        -d "username=$un&password=TestPass123!&email=${un}@test.com" >/dev/null
    local tok
    tok=$(get_user_token "$BASE_URL" "$un" "TestPass123!")
    [ -n "$tok" ] || { echo "    skipped: could not get token"; return 1; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $tok" \
        "$BASE_URL/api/admin/dashboard/stats")
    if [ "$code" = "403" ] || [ "$code" = "401" ]; then
        echo "    Correctly denied non-admin: $code"
    else
        echo "    Got status: $code"
    fi
}
run_test "Test 43: GET /api/admin/dashboard/stats - Non-admin denied" test_43

# Test 44: backchannel_logout_uri set / get / clear lifecycle (B1)
test_44() {
    local r
    r=$(curl -s -X PUT -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"backchannel_logout_uri":"https://rp-bc.example.com/backchannel-logout"}' \
        "$BASE_URL/api/admin/clients/fulla-portal")
    assert_json_field "$r" "status" "success" || return 1
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/clients/fulla-portal")
    assert_json_field "$r" "backchannel_logout_uri" "https://rp-bc.example.com/backchannel-logout" || return 1
    # Empty string clears the registration (NULL in DB, "" in the response).
    r=$(curl -s -X PUT -H "$(auth_header)" -H "Content-Type: application/json" \
        -d '{"backchannel_logout_uri":""}' "$BASE_URL/api/admin/clients/fulla-portal")
    assert_json_field "$r" "status" "success" || return 1
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/clients/fulla-portal")
    assert_json_field "$r" "backchannel_logout_uri" "" || return 1
    echo "    set -> read -> cleared: ok"
}
run_test "Test 44: PUT/GET/clear backchannel_logout_uri (B1)" test_44

# Test 45: backchannel_logout_uri must be https (B1, OIDC §2.3).
# NOTE: plain http:// is NOT a reliable negative case here because the test
# configs enable auth.allow_http_redirect_uri (dev hatch honored by the
# validator). ftp:// is invalid regardless of the hatch.
test_45() {
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT -H "$(auth_header)" \
        -H "Content-Type: application/json" \
        -d '{"backchannel_logout_uri":"ftp://rp.example.com/backchannel-logout"}' \
        "$BASE_URL/api/admin/clients/fulla-portal")
    assert_status "$code" "400" || return 1
    echo "    Correctly returned 400 for non-https backchannel_logout_uri"
}
run_test "Test 45: PUT backchannel_logout_uri - non-https scheme (400)" test_45

# Test 46: create a client WITH backchannel_logout_uri, read it back, clean up (B1)
test_46() {
    local ts
    ts=$(date +%s)
    local r
    r=$(curl -s -X POST -H "$(auth_header)" -H "Content-Type: application/json" \
        -d "{\"name\":\"BC Test Client $ts\",\"redirect_uris\":\"http://localhost:3100/callback\",\"allowed_grant_types\":\"authorization_code\",\"client_type\":\"CONFIDENTIAL\",\"backchannel_logout_uri\":\"https://rp-bc-create.example.com/bc\"}" \
        "$BASE_URL/api/admin/clients")
    assert_json_field "$r" "status" "success" || return 1
    local bc_id
    bc_id=$(echo "$r" | jq -r '.client_id')
    [ -n "$bc_id" ] && [ "$bc_id" != "null" ] || { echo "    missing client_id"; return 1; }
    r=$(curl -s -H "$(auth_header)" "$BASE_URL/api/admin/clients/$bc_id")
    assert_json_field "$r" "backchannel_logout_uri" "https://rp-bc-create.example.com/bc" || { \
        curl -s -X DELETE -H "$(auth_header)" "$BASE_URL/api/admin/clients/$bc_id" >/dev/null; return 1; }
    curl -s -X DELETE -H "$(auth_header)" "$BASE_URL/api/admin/clients/$bc_id" >/dev/null
    echo "    created with uri, read back, deleted: $bc_id"
}
run_test "Test 46: POST /api/admin/clients - create with backchannel_logout_uri (B1)" test_46

# Post-test Cleanup
echo "========================================"
echo "Post-test Cleanup"
echo "========================================"
reset_admin_account
echo ""

print_summary $TOTAL
