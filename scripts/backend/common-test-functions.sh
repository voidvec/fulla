#!/usr/bin/env bash
# common-test-functions.sh - Shared bash functions for API test scripts
# Source this file; do not execute directly.

# Database defaults
DB_USER="${FULLA_DB_USER:-fulla_user}"
DB_NAME="${FULLA_DB_NAME:-fulla_db}"
DB_PASSWORD="${FULLA_DB_PASSWORD:-123456}"
DB_HOST="${FULLA_DB_HOST:-localhost}"
DB_PORT="${FULLA_DB_PORT:-5432}"

# Test counters
TEST_PASSED=0
TEST_FAILED=0

# Colors
C_CYAN='\033[0;36m'
C_GREEN='\033[0;32m'
C_RED='\033[0;31m'
C_YELLOW='\033[1;33m'
C_NC='\033[0m'

# ---------------------------------------------------------------------------
# PKCE (RFC 7636) helpers — required since F-011/RFC 9700 §2.1.1 made PKCE
# mandatory for PUBLIC clients. Both fulla-portal and fulla-admin-console are PUBLIC,
# so every authorization-code login must carry code_challenge and every token
# exchange must carry the matching code_verifier.
# ---------------------------------------------------------------------------

# generate_pkce_verifier — emits a cryptographically-random code_verifier (43-128
# chars of the unreserved set [A-Za-z0-9-._~]). Uses openssl for the randomness.
generate_pkce_verifier() {
    # 32 random bytes -> base64url -> strip padding -> unreserved-only (~64 chars).
    openssl rand 32 | openssl base64 -A | tr '+/' '-_' | tr -d '='
}

# pkce_s256_challenge <verifier> — computes the S256 code_challenge:
# base64url(SHA256(verifier)) with padding stripped (RFC 7636 §4.2).
pkce_s256_challenge() {
    local verifier="$1"
    printf '%s' "$verifier" | openssl dgst -sha256 -binary | openssl base64 -A | tr '+/' '-_' | tr -d '='
}

# assert_status <actual_code> <expected_code> <context>
assert_status() {
    local actual="$1"
    local expected="$2"
    local context="${3:-}"
    if [ "$actual" != "$expected" ]; then
        echo -e "    ${C_RED}[FAIL] Expected HTTP $expected, got $actual ${context}${C_NC}"
        return 1
    fi
    return 0
}

# assert_json_field <json> <field> <expected_value>
assert_json_field() {
    local json="$1"
    local field="$2"
    local expected="$3"
    local actual
    actual=$(echo "$json" | jq -r ".$field" 2>/dev/null)
    if [ "$actual" != "$expected" ]; then
        echo -e "    ${C_RED}[FAIL] .$field: expected '$expected', got '$actual'${C_NC}"
        return 1
    fi
    return 0
}

# assert_json_exists <json> <field>
assert_json_exists() {
    local json="$1"
    local field="$2"
    local val
    val=$(echo "$json" | jq -r ".$field" 2>/dev/null)
    if [ -z "$val" ] || [ "$val" = "null" ]; then
        echo -e "    ${C_RED}[FAIL] missing field: .$field${C_NC}"
        return 1
    fi
    return 0
}

# curl_json <method> <url> [data] [extra_curl_args...]
# Returns: body on stdout, sets LAST_HTTP_CODE
curl_json() {
    local method="$1"
    local url="$2"
    local data="${3:-}"
    shift 3 || shift $#
    local extra_args=("$@")

    local tmp_file
    tmp_file=$(mktemp)

    local curl_args=(-s -w '\n%{http_code}' -X "$method" "$url")
    curl_args+=(-H "Content-Type: application/json")

    if [ -n "$data" ]; then
        curl_args+=(-d "$data")
    fi

    if [ ${#extra_args[@]} -gt 0 ]; then
        curl_args+=("${extra_args[@]}")
    fi

    local response
    response=$(curl "${curl_args[@]}" 2>/dev/null) || true

    LAST_HTTP_CODE=$(echo "$response" | tail -1)
    echo "$response" | sed '$d'
}

# curl_form <method> <url> <form_data_string> [extra_curl_args...]
# form_data_string: "key1=val1&key2=val2"
curl_form() {
    local method="$1"
    local url="$2"
    local form_data="$3"
    shift 3 || shift $#
    local extra_args=("$@")

    local curl_args=(-s -w '\n%{http_code}' -X "$method" "$url")
    curl_args+=(-H "Content-Type: application/x-www-form-urlencoded")
    curl_args+=(--data-urlencode "" -d "$form_data")

    if [ ${#extra_args[@]} -gt 0 ]; then
        curl_args+=("${extra_args[@]}")
    fi

    local response
    response=$(curl -s -w '\n%{http_code}' -X "$method" "$url" \
        -d "$form_data" "${extra_args[@]}" 2>/dev/null) || true

    LAST_HTTP_CODE=$(echo "$response" | tail -1)
    echo "$response" | sed '$d'
}

# run_test <name> <test_function>
run_test() {
    local name="$1"
    local func="$2"
    echo -e "${C_CYAN}[*] $name${C_NC}"
    if $func; then
        TEST_PASSED=$((TEST_PASSED + 1))
        echo -e "    ${C_GREEN}[PASS]${C_NC}"
    else
        TEST_FAILED=$((TEST_FAILED + 1))
        echo -e "    ${C_RED}[FAIL]${C_NC}"
    fi
    echo ""
}

# get_postgres_container - find THIS project's postgres container name.
# Matching only "postgres" grabbed ANY postgres container on the machine
# (e.g. an unrelated "ory-bench-postgres") and ran the admin reset against
# the wrong database. The compose service is "fulla-postgres"
# (deploy/docker/docker-compose.yml); no match -> caller falls back to the
# local-psql branch.
get_postgres_container() {
    docker ps --format "{{.Names}}" 2>/dev/null | grep -iE 'oauth2.*postgres|postgres.*oauth2' | head -1
}

# run_psql <query> - run psql against the database (Docker or local)
run_psql() {
    local query="$1"
    local container
    container=$(get_postgres_container)

    if [ -n "$container" ]; then
        docker exec "$container" psql -U "$DB_USER" -d "$DB_NAME" -c "$query" 2>/dev/null
    else
        PGPASSWORD="$DB_PASSWORD" psql -U "$DB_USER" -d "$DB_NAME" -h "$DB_HOST" -p "$DB_PORT" -c "$query" 2>/dev/null
    fi
}

# reset_admin_account - reset admin password and lockout
reset_admin_account() {
    echo -e "${C_CYAN}Resetting admin account (password + lockout)...${C_NC}"
    local hash='$pbkdf2-sha256$310000$61646d696e5f736565645f73616c74$6c0307305e1390e1214b15f1f4d0250b2de86aa0e8aa0e008e5cca03084d3d62'
    local salt=""
    local query="UPDATE users SET password_hash = '$hash', salt = '$salt', failed_login_count = 0, locked_until = 0 WHERE username = 'admin';"
    if run_psql "$query" >/dev/null 2>&1; then
        echo -e "${C_GREEN}Admin account reset successfully${C_NC}"
    else
        echo -e "${C_YELLOW}Warning: Could not reset admin account${C_NC}"
    fi
}

# assert_status_in <actual_code> "code1|code2|..." <context>
assert_status_in() {
    local actual="$1"
    local expected="$2"
    local context="${3:-}"
    local found=false
    IFS='|' read -ra codes <<< "$expected"
    for c in "${codes[@]}"; do
        if [ "$actual" = "$c" ]; then found=true; break; fi
    done
    if [ "$found" = "false" ]; then
        echo -e "    ${C_RED}[FAIL] Expected HTTP $expected, got $actual ${context}${C_NC}"
        return 1
    fi
    return 0
}

# get_user_token <base_url> <username> <password>
# Returns access_token on stdout.
# F-011/RFC 7636 (RFC 9700 §2.1.1): PKCE is mandatory for PUBLIC clients.
# fulla-portal is PUBLIC, so the login MUST carry code_challenge (S256) and the
# token exchange MUST carry the matching code_verifier. Without PKCE the login
# redirects with error=invalid_request (code_challenge required).
get_user_token() {
    local base_url="$1" username="$2" password="$3"
    local state="token-${username}-$$"
    # PKCE S256: generate a random verifier + its challenge.
    local verifier challenge
    verifier=$(generate_pkce_verifier)
    challenge=$(pkce_s256_challenge "$verifier")
    local login_resp
    login_resp=$(curl -s -X POST "$base_url/oauth2/login" \
        -d "username=$username&password=$password&client_id=fulla-portal&redirect_uri=http://127.0.0.1:5173/callback&scope=openid+profile&state=$state&code_challenge=$challenge&code_challenge_method=S256&json=true")
    local code
    code=$(echo "$login_resp" | jq -r '.code')
    [ -n "$code" ] && [ "$code" != "null" ] || return 1
    local tok_resp
    # F-017: fulla-portal is seeded token_endpoint_auth_method='none' (PUBLIC);
    # the 'none' method REJECTS any client_secret, so none is sent. PKCE
    # code_verifier is the client-authentication substitute for PUBLIC clients.
    tok_resp=$(curl -s -X POST "$base_url/oauth2/token" \
        -d "grant_type=authorization_code&code=$code&redirect_uri=http://127.0.0.1:5173/callback&client_id=fulla-portal&code_verifier=$verifier")
    echo "$tok_resp" | jq -r '.access_token'
}

# get_admin_token <base_url> <username> <password>
# Gets admin-scoped token via fulla-admin-console client (also a PUBLIC client, so
# PKCE applies the same way as get_user_token).
get_admin_token() {
    local base_url="$1" username="$2" password="$3"
    local state="adm-$$"
    local verifier challenge
    verifier=$(generate_pkce_verifier)
    challenge=$(pkce_s256_challenge "$verifier")
    local login_resp
    login_resp=$(curl -s -X POST "$base_url/oauth2/login" \
        -d "username=$username&password=$password&client_id=fulla-admin-console&redirect_uri=http://localhost:5174/admin/callback&scope=openid+profile+admin&state=$state&code_challenge=$challenge&code_challenge_method=S256&json=true")
    local code
    code=$(echo "$login_resp" | jq -r '.code')
    [ -n "$code" ] && [ "$code" != "null" ] || return 1
    local tok_resp
    tok_resp=$(curl -s -X POST "$base_url/oauth2/token" \
        -d "grant_type=authorization_code&code=$code&redirect_uri=http://localhost:5174/admin/callback&client_id=fulla-admin-console&client_secret=&code_verifier=$verifier")
    echo "$tok_resp" | jq -r '.access_token'
}

# print_summary <total>
print_summary() {
    local total="${1:-$((TEST_PASSED + TEST_FAILED))}"
    echo "========================================"
    echo "Test Summary: $TEST_PASSED/$total passed, $TEST_FAILED failed"
    echo "========================================"
    if [ $TEST_FAILED -gt 0 ]; then
        echo -e "${C_RED}FAILED${C_NC}"
        return 1
    else
        echo -e "${C_GREEN}ALL PASSED${C_NC}"
        return 0
    fi
}
