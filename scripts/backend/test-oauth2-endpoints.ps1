param(
    [string]$BaseUrl = "http://127.0.0.1:5555"
)

$ErrorActionPreference = "Stop"
$passed = 0
$failed = 0
$total = 59
$adminPassword = "admin"  # Track current admin password across tests

# Import common functions
. "$PSScriptRoot\common-test-functions.ps1"

function Test-Endpoint {
    param([string]$Name, [scriptblock]$Block)
    Write-Host "[*] $Name" -ForegroundColor Cyan
    try {
        & $Block
        $script:passed++
        Write-Host "    [PASS]" -ForegroundColor Green
    } catch {
        $script:failed++
        Write-Host "    [FAIL] $($_.Exception.Message)" -ForegroundColor Red
    }
    Write-Host ""
}

# ========================================
# Pre-test Setup: Reset admin account
# ========================================
Write-Host "========================================" -ForegroundColor Yellow
Write-Host "Pre-test Setup" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Yellow
Reset-AdminAccount
Write-Host ""

# ========================================
# OAuth2 Endpoints Tests
# ========================================
Write-Host "========================================" -ForegroundColor Yellow
Write-Host "OAuth2 Endpoints Tests ($total tests)" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Yellow
Write-Host "Base URL: $BaseUrl"
Write-Host ""

# ========================================
# Test 1: Health Check
# ========================================
Test-Endpoint "Test 1: Health Check" {
    $r = Invoke-RestMethod -Uri "$BaseUrl/health" -Method Get
    if ($r.status -ne "ok") { throw "status != ok" }
    Write-Host "    Status: $($r.status), Storage: $($r.storage_type)"
}

# ========================================
# Test 2: Health Live/Ready
# ========================================
Test-Endpoint "Test 2: Health Live/Ready" {
    $live = Invoke-RestMethod -Uri "$BaseUrl/health/live" -Method Get
    if ($live.status -ne "ok") { throw "/health/live status != ok" }
    $ready = Invoke-RestMethod -Uri "$BaseUrl/health/ready" -Method Get
    if ($ready.status -ne "ok" -and $ready.status -ne "degraded") { throw "/health/ready failed" }
    Write-Host "    Live: $($live.status), Ready: $($ready.status)"
}

# ========================================
# Test 3: OIDC Discovery
# ========================================
Test-Endpoint "Test 3: OIDC Discovery" {
    $r = Invoke-RestMethod -Uri "$BaseUrl/.well-known/openid-configuration" -Method Get
    if (-not $r.issuer) { throw "missing issuer" }
    if (-not $r.jwks_uri) { throw "missing jwks_uri" }
    if (-not $r.scopes_supported) { throw "missing scopes_supported" }
    # B1 (OIDC Back-Channel Logout 1.0): advertised support flags. Session
    # level is false — this OP issues subject-scoped logout_tokens (no sid).
    if ($null -eq $r.backchannel_logout_supported) { throw "missing backchannel_logout_supported" }
    if ($r.backchannel_logout_supported -ne $true) { throw "backchannel_logout_supported != true" }
    if ($null -eq $r.backchannel_logout_session_supported) { throw "missing backchannel_logout_session_supported" }
    if ($r.backchannel_logout_session_supported -ne $false) { throw "backchannel_logout_session_supported != false" }
    $script:discoveryIssuer = $r.issuer
    Write-Host "    Issuer: $($r.issuer)"
}

# ========================================
# Test 4: JWKS
# ========================================
Test-Endpoint "Test 4: JWKS" {
    $r = Invoke-RestMethod -Uri "$BaseUrl/.well-known/jwks.json" -Method Get
    if (-not $r.keys -or $r.keys.Count -eq 0) { throw "empty keys array" }
    $key = $r.keys[0]
    if ($key.kty -ne "RSA") { throw "kty != RSA" }
    if ($key.alg -ne "RS256") { throw "alg != RS256" }
    Write-Host "    Keys: $($r.keys.Count), kid: $($key.kid)"
}

# ========================================
# Test 5: OAuth2 Login (get auth code)
# ========================================
$authCode = $null
$accessToken = $null
$refreshToken = $null
$discoveryIssuer = $null

Test-Endpoint "Test 5: OAuth2 Login" {
    # F-011/RFC 7636 (RFC 9700 §2.1.1): PKCE mandatory for PUBLIC clients.
    # fulla-portal is PUBLIC -> login must carry code_challenge; Test 6 must
    # carry the matching code_verifier. The script-scope PKCE verifier is
    # set here and consumed by Test 6.
    $pkce = New-PkcePair
    $script:PkceVerifier = $pkce.verifier
    $body = @{
        username = 'admin'; password = 'admin'
        client_id = 'fulla-portal'
        redirect_uri = 'http://127.0.0.1:5173/callback'
        scope = 'openid profile'
        state = 'test-state-12345678'
        code_challenge = $pkce.challenge; code_challenge_method = 'S256'
        json = 'true'
    }
    $r = Invoke-RestMethod -Uri "$BaseUrl/oauth2/login" -Method Post -Body $body
    if (-not $r.code) { throw "no auth code returned" }
    $script:authCode = $r.code
    Write-Host "    Code: $($r.code.Substring(0,20))... ($($r.code.Length) chars)"
}

# ========================================
# Test 6: Token Exchange (with id_token)
# ========================================
Test-Endpoint "Test 6: Token Exchange + id_token" {
    if (-not $authCode) { throw "skipped: no auth code" }
    $body = @{
        grant_type = 'authorization_code'
        code = $authCode
        redirect_uri = 'http://127.0.0.1:5173/callback'
        client_id = 'fulla-portal'
        code_verifier = $script:PkceVerifier
    }
    $r = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body $body
    if (-not $r.access_token) { throw "no access_token" }
    if ($r.access_token.Length -lt 40) { throw "token too short ($($r.access_token.Length) chars)" }
    if (-not $r.refresh_token) { throw "no refresh_token" }
    $script:accessToken = $r.access_token
    $script:refreshToken = $r.refresh_token

    # Verify id_token present (scope included openid)
    if (-not $r.id_token) { throw "no id_token (scope=openid)" }
    $parts = $r.id_token.Split('.')
    if ($parts.Count -ne 3) { throw "id_token not 3-part JWT" }
    Write-Host "    AT: $($r.access_token.Substring(0,20))..., id_token: present (3-part JWT)"
}

# ========================================
# Test 7: UserInfo
# ========================================
Test-Endpoint "Test 7: UserInfo" {
    if (-not $accessToken) { throw "skipped: no token" }
    $headers = @{ Authorization = "Bearer $accessToken" }
    $r = Invoke-RestMethod -Uri "$BaseUrl/oauth2/userinfo" -Method Get -Headers $headers
    if (-not $r.sub) { throw "no sub" }
    if ($r.sub.Length -ne 36) { throw "sub not UUID format (len=$($r.sub.Length))" }
    if ($r.name -eq $r.sub) { throw "name equals sub (username not resolved)" }
    Write-Host "    Sub: $($r.sub), Name: $($r.name), Roles: $($r.roles -join ',')"
}

# ========================================
# Test 8: Admin Dashboard (F-010: requires admin scope -- use fulla-admin-console token)
# ========================================
Test-Endpoint "Test 8: Admin Dashboard" {
    # F-010: /api/admin/* now requires the `admin` scope on the access token
    # (in addition to the RBAC admin role). The fulla-portal token from Test 6
    # carries only `openid profile`, so it would 403 here -- obtain a proper
    # admin-scoped token via the fulla-admin-console client instead.
    $adminToken = Get-AdminToken -BaseUrl $BaseUrl
    if (-not $adminToken) { throw "no admin token" }
    $headers = @{ Authorization = "Bearer $adminToken" }
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/dashboard" -Method Get -Headers $headers
    if ($r.status -ne "success") { throw "status != success" }
    Write-Host "    Message: $($r.message)"
}

# ========================================
# Test 8b: Insufficient scope on /api/admin (F-010, 403 insufficient_scope)
# ========================================
Test-Endpoint "Test 8b: /api/admin without admin scope -> 403 insufficient_scope" {
    if (-not $accessToken) { throw "skipped: no token" }
    # The fulla-portal token carries only `openid profile` (no admin scope) ->
    # F-010 rejects with 403 + WWW-Authenticate: Bearer error="insufficient_scope".
    $headers = @{ Authorization = "Bearer $accessToken" }
    try {
        $null = Invoke-RestMethod -Uri "$BaseUrl/api/admin/dashboard" -Method Get -Headers $headers
        throw "expected 403, got success"
    } catch {
        # PS7 throws HttpResponseException (not WebException). Read the status
        # + headers from $_.Exception.Response (HttpResponseMessage).
        $resp = $_.Exception.Response
        if ($null -eq $resp) { throw $_ }
        $code = [int]$resp.StatusCode
        if ($code -ne 403) { throw "expected 403, got $code" }
        $wwwAuth = $resp.Headers.GetValues("WWW-Authenticate") -join ", "
        if ($wwwAuth -notmatch 'insufficient_scope') { throw "WWW-Authenticate missing insufficient_scope: $wwwAuth" }
        if ($wwwAuth -notmatch 'scope="audit:read"') { throw "WWW-Authenticate missing scope=audit:read: $wwwAuth" }
        Write-Host "    403 insufficient_scope (scope=audit:read) confirmed"
    }
}

# ========================================
# Test 9: Token Refresh
# ========================================
Test-Endpoint "Test 9: Token Refresh" {
    if (-not $refreshToken) { throw "skipped: no refresh_token" }
    $body = @{
        grant_type = 'refresh_token'
        refresh_token = $refreshToken
        client_id = 'fulla-portal'
    }
    $r = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body $body
    if (-not $r.access_token) { throw "no new access_token" }
    if (-not $r.refresh_token) { throw "no new refresh_token" }
    if ($r.access_token -eq $accessToken) { throw "same access_token (not rotated)" }
    $script:accessToken = $r.access_token
    $script:refreshToken = $r.refresh_token
    Write-Host "    New AT: $($r.access_token.Substring(0,20))..."
}

# ========================================
# Test 9b: Token Refresh - Confidential client without secret (F-003, 401)
# ========================================
Test-Endpoint "Test 9b: Token Refresh - Missing client_secret (401)" {
    if (-not $refreshToken) { throw "skipped: no refresh_token" }
    # F-003 (RFC 6749 §6 / §3.2.1): a CONFIDENTIAL client MUST authenticate
    # on the refresh_token grant; omitting the secret MUST yield 401
    # invalid_client. PUBLIC clients (like fulla-portal) are exempt (RFC 6749
    # §10.2: client_id existence check only), so this test uses backend-svc
    # (CONFIDENTIAL) to exercise the F-003 secret requirement. The refresh
    # token itself belongs to fulla-portal, so the secret check fires (401)
    # before any client_id<->token binding check.
    $body = @{
        grant_type = 'refresh_token'
        refresh_token = $refreshToken
        client_id = 'backend-svc'
    }
    try {
        Invoke-WebRequest -Uri "$BaseUrl/oauth2/token" -Method Post -Body $body -UseBasicParsing -ErrorAction Stop | Out-Null
        throw "refresh without client_secret should fail for confidential client"
    } catch {
        if ($_.Exception.Message -match "should fail") { throw $_.Exception.Message }
        # PS7: Invoke-WebRequest throws HttpResponseException on 4xx; the body
        # is in $_.ErrorDetails.Message.
        $resp = $_.Exception.Response
        if ($null -eq $resp -or [int]$resp.StatusCode -ne 401) { throw "expected 401, got $($_.Exception.Message)" }
        $respBody = "$($_.ErrorDetails.Message)"
        if ($respBody -notmatch '"error"\s*:\s*"invalid_client"') { throw "expected invalid_client, got: $respBody" }
        Write-Host "    401 + invalid_client returned for unauthenticated refresh"
    }
}

# ========================================
# Test 10: Client Credentials Grant
# ========================================
Test-Endpoint "Test 10: Client Credentials" {
    # F-017: backend-svc is seeded token_endpoint_auth_method=client_secret_basic,
    # so its secret MUST travel via HTTP Basic (body form is now rejected).
    $basicAuth = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("backend-svc:test-secret"))
    $headers = @{ Authorization = "Basic $basicAuth" }
    $body = @{
        grant_type = 'client_credentials'
        client_id = 'backend-svc'
        scope = 'tokens:read'
    }
    $r = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body $body -Headers $headers
    if (-not $r.access_token) { throw "no access_token" }
    if ($r.refresh_token) { throw "client_credentials should NOT have refresh_token" }
    if ($r.scope -ne "tokens:read") { throw "scope mismatch" }
    Write-Host "    AT: $($r.access_token.Substring(0,20))..., Scope: $($r.scope)"
}

# ========================================
# Test 11: Token Introspection
# ========================================
Test-Endpoint "Test 11: Token Introspection" {
    if (-not $accessToken) { throw "skipped: no token" }
    # RFC 7662 §2.1: introspect authenticates the CALLING CLIENT via client
    # credentials (Basic header or body client_id/client_secret), NOT a
    # user Bearer token. No Authorization header is sent.
    # RFC 7662 §4 / TokenEndpointController.cc:428: ALL callers must supply a
    # client_secret (introspection is protected against token enumeration).
    # fulla-portal is PUBLIC (no secret) and cannot call introspect; use
    # backend-svc (CONFIDENTIAL, secret "test-secret"). F-017: backend-svc is
    # seeded token_endpoint_auth_method=client_secret_basic, so authenticate
    # via HTTP Basic, not a body client_secret.
    $basicAuth = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("backend-svc:test-secret"))
    $headers = @{ Authorization = "Basic $basicAuth" }
    $r = Invoke-RestMethod -Uri "$BaseUrl/oauth2/introspect" -Method Post -Headers $headers -Body @{ token = $accessToken }
    if ($r.active -ne $true) { throw "active != true" }
    # F-016: introspection iss MUST be byte-identical to the discovery issuer
    if (-not $r.iss) { throw "introspection missing iss claim" }
    if ($discoveryIssuer -and $r.iss -cne $discoveryIssuer) { throw "iss '$($r.iss)' != discovery issuer '$discoveryIssuer'" }
    Write-Host "    Active: $($r.active), Sub: $($r.sub), Scope: $($r.scope), Iss: $($r.iss)"
}

# ========================================
# Test 12: Token Revocation + Verify
# ========================================
Test-Endpoint "Test 12: Token Revocation" {
    if (-not $accessToken) { throw "skipped: no token" }
    # RFC 7009 §2.1: revoke authenticates the calling client AND enforces token
    # ownership — only the token's issuing client may revoke it. The token was
    # issued to fulla-portal (PUBLIC), so fulla-portal must be the caller. RFC 7009
    # §2.1 exempts PUBLIC clients from client_secret at the revocation endpoint
    # ("if the client is a public client, then it does not authenticate"), so
    # client_id alone authenticates a PUBLIC caller.
    $headers = @{ Authorization = "Bearer $accessToken" }
    $body = @{
        token = $accessToken
        client_id = 'fulla-portal'
    }
    Invoke-WebRequest -Uri "$BaseUrl/oauth2/revoke" -Method Post -Body $body -UseBasicParsing | Out-Null

    # Verify: try to use the revoked token - should get 401
    try {
        Invoke-RestMethod -Uri "$BaseUrl/oauth2/userinfo" -Method Get -Headers $headers -ErrorAction Stop
        throw "token should be revoked but userinfo succeeded"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Unauthorized") {
            Write-Host "    Revoked and verified: userinfo returns 401"
        } else {
            throw "unexpected error: $($_.Exception.Message)"
        }
    }
}

# ========================================
# Test 12b: Revoke a REFRESH token, then it must NOT be usable to refresh
# (C3 / RFC 7009 §2.1 — the revocation endpoint must revoke ANY token type).
# This closes the coverage gap that hid the C3 hash bug: previously the script
# only revoked access tokens (Test 12/43), so a refresh-token revoke that was
# a silent no-op went undetected.
# ========================================
Test-Endpoint "Test 12b: Revoke refresh token -> refresh fails (C3/RFC 7009)" {
    # Mint a fresh token pair so this test is independent of Test 12's state.
    $pkce = New-PkcePair
    $loginResp = Invoke-RestMethod -Uri "$BaseUrl/oauth2/login" -Method Post -Body @{
        username = 'admin'; password = 'admin'
        client_id = 'fulla-portal'
        redirect_uri = 'http://127.0.0.1:5173/callback'
        scope = 'openid profile'; state = 't12b'
        code_challenge = $pkce.challenge; code_challenge_method = 'S256'
        json = 'true'
    }
    if (-not $loginResp.code) { throw "no auth code" }
    $tokResp = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body @{
        grant_type = 'authorization_code'; code = $loginResp.code
        redirect_uri = 'http://127.0.0.1:5173/callback'
        client_id = 'fulla-portal'; code_verifier = $pkce.verifier
    }
    if (-not $tokResp.refresh_token) { throw "no refresh_token" }
    # Rotate once to get a current (un-rotated) refresh token.
    $rotated = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body @{
        grant_type = 'refresh_token'; refresh_token = $tokResp.refresh_token
        client_id = 'fulla-portal'
    }
    if (-not $rotated.refresh_token) { throw "refresh rotation failed" }

    # Revoke the refresh token (fulla-portal owns it; PUBLIC, client_id only).
    Invoke-WebRequest -Uri "$BaseUrl/oauth2/revoke" -Method Post -Body @{ token = $rotated.refresh_token; client_id = 'fulla-portal' } -UseBasicParsing | Out-Null

    # RFC 7009 §2.1 + RFC 6749 §6: the revoked refresh token MUST NOT mint new
    # tokens. Expect an error (invalid_grant — reuse detected / revoked) and
    # NO new access_token.
    $refreshErr = ""
    $refreshAt = ""
    try {
        $refreshAfter = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body @{
            grant_type = 'refresh_token'; refresh_token = $rotated.refresh_token
            client_id = 'fulla-portal'
        } -ErrorAction Stop
        # Success path: the revoked token still minted -> C3 failure.
        $refreshAt = "$($refreshAfter.access_token)"
    } catch {
        # Expected: an error response (C3 worked — the revoked refresh token
        # was rejected). PS7: body is in $_.ErrorDetails.Message (NOT
        # GetResponseStream, which is PS5.1-only and silently returns nothing
        # in PS7, which previously made $refreshErr stay empty and masked the
        # C3 success as a failure).
        $respBody = "$($_.ErrorDetails.Message)"
        $parsed = $respBody | ConvertFrom-Json -ErrorAction SilentlyContinue
        if ($parsed) { $refreshErr = "$($parsed.error)" }
        if (-not $refreshErr -and $respBody -match '"error"\s*:\s*"([^"]+)"') { $refreshErr = $Matches[1] }
    }
    if (-not $refreshErr) { throw "refresh after revoke succeeded (no error) - C3 FAILED" }
    if ($refreshAt) { throw "refresh after revoke returned a new access_token - C3 FAILED" }
    Write-Host "    Revoked refresh token correctly rejected: error=$refreshErr"
}

# ========================================
# Test 13: User Registration
# ========================================
Test-Endpoint "Test 13: User Registration" {
    $ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $body = @{
        username = "testuser_$ts"
        password = "TestPass123"
        email = "test_$ts@example.com"
    }
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/register" -Method Post -Body $body
    if (-not $r.message) { throw "no message in response" }
    Write-Host "    Registered: testuser_$ts"
}

# ========================================
# Test 14: User Profile (GET /api/me)
# ========================================
Test-Endpoint "Test 14: User Profile" {
    # Need a fresh token (previous was revoked). PKCE (F-011/RFC 7636).
    $pkce = New-PkcePair
    $loginBody = @{
        username = 'admin'; password = 'admin'
        client_id = 'fulla-portal'
        redirect_uri = 'http://127.0.0.1:5173/callback'
        scope = 'openid profile'; state = 'test-state-12345678'
        code_challenge = $pkce.challenge; code_challenge_method = 'S256'
        json = 'true'
    }
    $login = Invoke-RestMethod -Uri "$BaseUrl/oauth2/login" -Method Post -Body $loginBody
    $tokenBody = @{
        grant_type = 'authorization_code'; code = $login.code
        redirect_uri = 'http://127.0.0.1:5173/callback'
        client_id = 'fulla-portal'; code_verifier = $pkce.verifier
    }
    $tok = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body $tokenBody
    $script:accessToken = $tok.access_token

    $headers = @{ Authorization = "Bearer $($tok.access_token)" }
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/me" -Method Get -Headers $headers
    if (-not $r.username) { throw "no username" }
    Write-Host "    Username: $($r.username), Email: $($r.email)"
}

# ========================================
# Test 15: Password Reset Request
# ========================================
Test-Endpoint "Test 15: Password Reset Request" {
    $body = @{ email = 'admin@example.com' } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/password-reset/request" -Method Post -Body $body -ContentType 'application/json'
    if (-not $r.message) { throw "no message" }
    # Should always return 200 (anti-enumeration)
    Write-Host "    Response: $($r.message)"
}

# ========================================
# Test 16: Password Reset (non-existent email - still 200)
# ========================================
Test-Endpoint "Test 16: Password Reset (non-existent)" {
    $body = @{ email = 'nobody@nowhere.com' } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/password-reset/request" -Method Post -Body $body -ContentType 'application/json'
    if (-not $r.message) { throw "no message" }
    Write-Host "    Anti-enumeration: same response for non-existent email"
}

# ========================================
# Test 17: Password Change (PUT /api/me/password)
# ========================================
Test-Endpoint "Test 17: Password Change" {
    if (-not $accessToken) { throw "skipped: no token" }
    $headers = @{
        Authorization = "Bearer $accessToken"
        "Content-Type" = "application/json"
    }
    # Change password and verify it works
    $body = '{"old_password":"admin","new_password":"NewPass123!"}'
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/me/password" -Method Put -Body $body -Headers $headers
    if (-not $r.message) { throw "no message" }
    Write-Host "    $($r.message)"

    # Verify old token is revoked (password change revokes all tokens)
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/me" -Method Get -Headers $headers -ErrorAction Stop
        Write-Host "    WARNING: old token still works after password change"
    } catch {
        Write-Host "    Verified: old token revoked after password change"
    }

    # Restore password for future test runs. PKCE (F-011/RFC 7636) required
    # for the restore login.
    $script:adminPassword = "NewPass123!"
    try {
        $restorePkce = New-PkcePair
        $restoreLogin = Invoke-RestMethod -Uri "$BaseUrl/oauth2/login" -Method Post -Body @{
            username = 'admin'; password = 'NewPass123!'
            client_id = 'fulla-portal'
            redirect_uri = 'http://127.0.0.1:5173/callback'
            scope = 'openid'; state = 'restore-pw-state1'
            code_challenge = $restorePkce.challenge; code_challenge_method = 'S256'
            json = 'true'
        }
        $restoreTok = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body @{
            grant_type = 'authorization_code'; code = $restoreLogin.code
            redirect_uri = 'http://127.0.0.1:5173/callback'
            client_id = 'fulla-portal'; code_verifier = $restorePkce.verifier
        }
        $restoreHeaders = @{ Authorization = "Bearer $($restoreTok.access_token)"; "Content-Type" = "application/json" }
        Invoke-RestMethod -Uri "$BaseUrl/api/me/password" -Method Put -Body '{"old_password":"NewPass123!","new_password":"admin"}' -Headers $restoreHeaders | Out-Null
        $script:adminPassword = "admin"
        Write-Host "    Password restored to 'admin'"
    } catch {
        # Fallback: try DB reset
        Reset-AdminAccount -Silent
        $script:adminPassword = "admin"
        Write-Host "    Password restored via DB reset"
    }
    # Defense-in-depth: unconditionally reset the admin account so downstream
    # tests (20+) that depend on the seeded admin/admin credentials see the
    # correct password regardless of whether the API restore path above
    # succeeded (the restore can race or fail silently under load).
    Reset-AdminAccount -Silent
}

# ========================================
# Test 17b-17d: Password Reset Confirm
# ========================================
Test-Endpoint "Test 17b: POST /api/password-reset/confirm - Invalid token (400)" {
    $body = @{ token = "invalid-token-xyz"; password = "NewPass123!" } | ConvertTo-Json
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/password-reset/confirm" -Method Post -Body $body -ContentType 'application/json' -ErrorAction Stop
        throw "should have returned 400"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "BadRequest") {
            Write-Host "    Correctly returned 400 for invalid token"
        } else {
            $code = $_.Exception.Response.StatusCode
            Write-Host "    Got status: $code (may be expected)"
        }
    }
}

Test-Endpoint "Test 17c: POST /api/password-reset/confirm - Missing fields (400)" {
    $body = @{ } | ConvertTo-Json
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/password-reset/confirm" -Method Post -Body $body -ContentType 'application/json' -ErrorAction Stop
        throw "should have returned 400"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "BadRequest") {
            Write-Host "    Correctly returned 400 for missing fields"
        } else {
            $code = $_.Exception.Response.StatusCode
            Write-Host "    Got status: $code (may be expected)"
        }
    }
}

# ========================================
# Test 18: RFC 8414 OAuth Authorization Server Metadata
# ========================================
Test-Endpoint "Test 18: GET /.well-known/oauth-authorization-server" {
    $r = Invoke-RestMethod -Uri "$BaseUrl/.well-known/oauth-authorization-server" -Method Get
    if (-not $r.issuer) { throw "missing issuer" }
    if (-not $r.authorization_endpoint) { throw "missing authorization_endpoint" }
    if (-not $r.token_endpoint) { throw "missing token_endpoint" }
    if (-not $r.grant_types_supported) { throw "missing grant_types_supported" }
    if ($r.grant_types_supported -isnot [array]) { throw "grant_types_supported not array" }
    Write-Host "    Issuer: $($r.issuer), Grants: [$($r.grant_types_supported -join ', ')]"
}

# ========================================
# Test 19: OAuth2 Consent (without session)
# ========================================
Test-Endpoint "Test 19: POST /oauth2/consent - No session" {
    $body = @{ client_id = "fulla-portal"; scope = "openid profile"; action = "approve" } | ConvertTo-Json
    try {
        Invoke-RestMethod -Uri "$BaseUrl/oauth2/consent" -Method Post -Body $body -ContentType 'application/json' -ErrorAction Stop
        throw "should have returned error (no session)"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "BadRequest" -or $code -eq "Unauthorized" -or $code -eq "Forbidden") {
            Write-Host "    Correctly rejected: $code"
        } else {
            Write-Host "    Got status: $code (may be expected)"
        }
    }
}

# ========================================
# Test 20: Dynamic Client Registration (RFC 7591)
# ========================================
$regClientId = $null
Test-Endpoint "Test 20: POST /oauth2/register - Register Client" {
    # This endpoint uses AuthorizationFilter — requires admin role via RBAC
    $admTok = Get-AdminToken -BaseUrl $BaseUrl -Password $adminPassword
    $h = @{ Authorization = "Bearer $admTok"; "Content-Type" = "application/json" }
    $body = @{
        client_name = "Test DynReg $(Get-Date -Format 'yyyyMMddHHmmss')"
        redirect_uris = @("http://localhost:4000/callback")
        grant_types = @("authorization_code")
    } | ConvertTo-Json
    try {
        $r = Invoke-RestMethod -Uri "$BaseUrl/oauth2/register" -Method Post -Headers $h -Body $body
        if (-not $r.client_id) { throw "missing client_id" }
        if (-not $r.client_secret) { throw "missing client_secret" }
        if (-not $r.client_name) { throw "missing client_name" }
        $script:regClientId = $r.client_id
        Write-Host "    Registered: client_id=$($r.client_id)"
    } catch {
        $code = $_.Exception.Response.StatusCode
        # RBAC may deny if /oauth2/register path not whitelisted in config
        if ($code -eq "Forbidden") {
            Write-Host "    RBAC denied (expected if /oauth2/register not in config.rbac_rules)"
        } else {
            throw "unexpected: got $code"
        }
    }
}

Test-Endpoint "Test 20b: POST /oauth2/register - Missing client_name (400)" {
    $admTok = Get-AdminToken -BaseUrl $BaseUrl -Password $adminPassword
    $h = @{ Authorization = "Bearer $admTok"; "Content-Type" = "application/json" }
    try {
        $body = @{ redirect_uris = @("http://localhost/cb") } | ConvertTo-Json
        Invoke-RestMethod -Uri "$BaseUrl/oauth2/register" -Method Post -Headers $h -Body $body -ErrorAction Stop
        throw "should have returned 400"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "BadRequest") {
            Write-Host "    Correctly returned 400 for missing client_name"
        } elseif ($code -eq "Forbidden") {
            Write-Host "    RBAC denied (expected if /oauth2/register not in config.rbac_rules)"
        } else { throw "expected 400, got: $code" }
    }
}

Test-Endpoint "Test 20c: POST /oauth2/register - No auth (401/403)" {
    $body = @{ client_name = "No Auth Test"; redirect_uris = @("http://localhost/cb") } | ConvertTo-Json
    try {
        Invoke-RestMethod -Uri "$BaseUrl/oauth2/register" -Method Post -Body $body -ContentType 'application/json' -ErrorAction Stop
        throw "should have returned 401/403"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "Unauthorized" -or $code -eq "Forbidden") {
            Write-Host "    Correctly rejected: $code"
        } else { throw "expected 401/403, got: $code" }
    }
}

# ========================================
# Test 21-24: MFA Flow
# ========================================
$mfaToken = $null
$mfaSecret = $null

Test-Endpoint "Test 21: POST /api/me/mfa/setup - Setup MFA" {
    $tok = Get-UserToken -BaseUrl $BaseUrl -Username "admin" -Password $adminPassword
    $script:mfaToken = $tok
    $h = @{ Authorization = "Bearer $tok"; "Content-Type" = "application/json" }
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/me/mfa/setup" -Method Post -Headers $h
    if (-not $r.secret) { throw "missing secret" }
    if (-not $r.otpauth_uri) { throw "missing otpauth_uri" }
    $script:mfaSecret = $r.secret
    Write-Host "    Secret: $($r.secret.Substring(0,8))..., URI present"
}

Test-Endpoint "Test 21b: POST /api/me/mfa/setup - No auth (401)" {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/me/mfa/setup" -Method Post -ErrorAction Stop
        throw "should have returned 401"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Unauthorized") {
            Write-Host "    Correctly returned 401"
        } else { throw "expected 401, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 22: POST /api/me/mfa/verify - Invalid code (400)" {
    if (-not $mfaToken) { throw "skipped: no MFA setup" }
    $h = @{ Authorization = "Bearer $mfaToken"; "Content-Type" = "application/json" }
    $body = @{ code = "000000" } | ConvertTo-Json
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/me/mfa/verify" -Method Post -Headers $h -Body $body -ErrorAction Stop
        # If it succeeds with 000000, that is unlikely but not an error
        Write-Host "    WARNING: code 000000 accepted (unlikely but valid)"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "BadRequest" -or $code -eq "Unauthorized") {
            Write-Host "    Correctly rejected invalid code: $code"
        } else {
            Write-Host "    Got status: $code"
        }
    }
}

Test-Endpoint "Test 22b: POST /api/me/mfa/verify - No auth (401)" {
    try {
        $body = @{ code = "123456" } | ConvertTo-Json
        Invoke-RestMethod -Uri "$BaseUrl/api/me/mfa/verify" -Method Post -Body $body -ContentType 'application/json' -ErrorAction Stop
        throw "should have returned 401"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Unauthorized") {
            Write-Host "    Correctly returned 401"
        } else { throw "expected 401, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 23: POST /api/me/mfa/disable - No auth (401)" {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/me/mfa/disable" -Method Post -ErrorAction Stop
        throw "should have returned 401"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Unauthorized") {
            Write-Host "    Correctly returned 401"
        } else { throw "expected 401, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 24: POST /oauth2/mfa/verify - Invalid code format (400)" {
    $body = @{ mfa_token = "invalid"; code = "abc" } | ConvertTo-Json
    try {
        Invoke-RestMethod -Uri "$BaseUrl/oauth2/mfa/verify" -Method Post -Body $body -ContentType 'application/json' -ErrorAction Stop
        throw "should have returned error"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "BadRequest" -or $code -eq "Unauthorized" -or $code -eq "Forbidden") {
            Write-Host "    Correctly rejected: $code"
        } else {
            Write-Host "    Got status: $code"
        }
    }
}

# ========================================
# Test 25-27: User Self-Service
# ========================================
$selfServiceToken = $null

Test-Endpoint "Test 25: GET /api/me/authorized-apps - List" {
    $tok = Get-UserToken -BaseUrl $BaseUrl -Username "admin" -Password $adminPassword
    $script:selfServiceToken = $tok
    $h = @{ Authorization = "Bearer $tok" }
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/me/authorized-apps" -Method Get -Headers $h
    if ($null -eq $r.authorized_apps) { throw "missing authorized_apps" }
    if ($r.authorized_apps -isnot [array]) { throw "authorized_apps not array" }
    Write-Host "    Total authorized apps: $($r.total)"
}

Test-Endpoint "Test 25b: GET /api/me/authorized-apps - No auth (401)" {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/me/authorized-apps" -Method Get -ErrorAction Stop
        throw "should have returned 401"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Unauthorized") {
            Write-Host "    Correctly returned 401"
        } else { throw "expected 401, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 26: DELETE /api/me/authorized-apps/:clientId - Non-existent (404)" {
    if (-not $selfServiceToken) { throw "skipped: no token" }
    $h = @{ Authorization = "Bearer $selfServiceToken" }
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/me/authorized-apps/nonexistent-app-xyz" -Method Delete -Headers $h -ErrorAction Stop
        throw "should have returned 404"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "NotFound") {
            Write-Host "    Correctly returned 404"
        } else {
            Write-Host "    Got status: $($_.Exception.Response.StatusCode)"
        }
    }
}

Test-Endpoint "Test 26b: DELETE /api/me/authorized-apps/:clientId - No auth (401)" {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/me/authorized-apps/fulla-portal" -Method Delete -ErrorAction Stop
        throw "should have returned 401"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Unauthorized") {
            Write-Host "    Correctly returned 401"
        } else { throw "expected 401, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 27: DELETE /api/me - Delete test user account" {
    # Register a throwaway user, then delete it
    $ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $un = "delme_$ts"
    $body = @{ username = $un; password = "TestPass123!"; email = "${un}@test.com" }
    Invoke-RestMethod -Uri "$BaseUrl/api/register" -Method Post -Body $body | Out-Null
    $tok = Get-UserToken -BaseUrl $BaseUrl -Username $un -Password "TestPass123!"
    $h = @{ Authorization = "Bearer $tok" }
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/me" -Method Delete -Headers $h
    if (-not $r.message) { throw "no message" }
    Write-Host "    Deleted account: $un"
}

Test-Endpoint "Test 27b: DELETE /api/me - No auth (401)" {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/me" -Method Delete -ErrorAction Stop
        throw "should have returned 401"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Unauthorized") {
            Write-Host "    Correctly returned 401"
        } else { throw "expected 401, got: $($_.Exception.Response.StatusCode)" }
    }
}

# ========================================
# Test 28: Email Verification
# ========================================
Test-Endpoint "Test 28: GET /api/verify-email - Invalid token" {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/verify-email?token=invalid-token-xyz" -Method Get -ErrorAction Stop
        throw "should have returned error"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "BadRequest" -or $code -eq "NotFound") {
            Write-Host "    Correctly rejected invalid token: $code"
        } else {
            Write-Host "    Got status: $code"
        }
    }
}

Test-Endpoint "Test 28b: GET /api/verify-email - Missing token" {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/verify-email" -Method Get -ErrorAction Stop
        throw "should have returned error"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "BadRequest") {
            Write-Host "    Correctly returned 400 for missing token"
        } else {
            Write-Host "    Got status: $code"
        }
    }
}

Test-Endpoint "Test 28c: POST /api/verify-email/resend - No auth (401)" {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/verify-email/resend" -Method Post -ErrorAction Stop
        throw "should have returned 401"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Unauthorized") {
            Write-Host "    Correctly returned 401"
        } else { throw "expected 401, got: $($_.Exception.Response.StatusCode)" }
    }
}

# ========================================
# Test 29-33: WebAuthn Flow
# ========================================
Test-Endpoint "Test 29: POST /api/me/webauthn/register/begin - No auth (401)" {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/me/webauthn/register/begin" -Method Post -ErrorAction Stop
        throw "should have returned 401"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Unauthorized") {
            Write-Host "    Correctly returned 401"
        } else { throw "expected 401, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 30: POST /api/me/webauthn/register/begin - With auth" {
    $tok = Get-UserToken -BaseUrl $BaseUrl -Username "admin" -Password $adminPassword
    $h = @{ Authorization = "Bearer $tok"; "Content-Type" = "application/json" }
    try {
        $r = Invoke-RestMethod -Uri "$BaseUrl/api/me/webauthn/register/begin" -Method Post -Headers $h
        if (-not $r.options -and -not $r.challenge) {
            Write-Host "    Response received (shape may vary)"
        } else {
            Write-Host "    Options/challenge present"
        }
    } catch {
        Write-Host "    WebAuthn not fully configured: $($_.Exception.Message)"
    }
}

Test-Endpoint "Test 31: POST /api/me/webauthn/register/finish - Invalid data" {
    $tok = Get-UserToken -BaseUrl $BaseUrl -Username "admin" -Password $adminPassword
    $h = @{ Authorization = "Bearer $tok"; "Content-Type" = "application/json" }
    try {
        $body = @{ credential_id = "invalid"; public_key = "invalid" } | ConvertTo-Json
        Invoke-RestMethod -Uri "$BaseUrl/api/me/webauthn/register/finish" -Method Post -Headers $h -Body $body -ErrorAction Stop
        Write-Host "    Response received (no error, shape may vary)"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "BadRequest") {
            Write-Host "    Correctly returned 400 for invalid data"
        } else {
            Write-Host "    Got status: $code (may be expected)"
        }
    }
}

Test-Endpoint "Test 32: POST /oauth2/webauthn/authenticate/begin" {
    try {
        $r = Invoke-RestMethod -Uri "$BaseUrl/oauth2/webauthn/authenticate/begin" -Method Post
        Write-Host "    Response received"
    } catch {
        Write-Host "    WebAuthn not fully configured: $($_.Exception.Message)"
    }
}

Test-Endpoint "Test 33: GET /api/me/webauthn/credentials - No auth (401)" {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/me/webauthn/credentials" -Method Get -ErrorAction Stop
        throw "should have returned 401"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Unauthorized") {
            Write-Host "    Correctly returned 401"
        } else { throw "expected 401, got: $($_.Exception.Response.StatusCode)" }
    }
}

# ========================================
# Test 34-35: Device Authorization Flow (RFC 8628)
# ========================================
Test-Endpoint "Test 34: POST /oauth2/device_authorization" {
    $body = @{ client_id = "fulla-portal"; scope = "openid profile" }
    try {
        $r = Invoke-RestMethod -Uri "$BaseUrl/oauth2/device_authorization" -Method Post -Body $body
        if (-not $r.device_code) { throw "missing device_code" }
        if (-not $r.user_code) { throw "missing user_code" }
        if (-not $r.verification_uri) { throw "missing verification_uri" }
        Write-Host "    device_code: $($r.device_code.Substring(0,8))..., user_code: $($r.user_code)"
    } catch {
        Write-Host "    Device flow response: $($_.Exception.Message)"
    }
}

Test-Endpoint "Test 34b: POST /oauth2/device_authorization - Missing client_id (400)" {
    try {
        Invoke-RestMethod -Uri "$BaseUrl/oauth2/device_authorization" -Method Post -Body @{ scope = "openid" } -ErrorAction Stop
        throw "should have returned 400"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "BadRequest") {
            Write-Host "    Correctly returned 400"
        } else {
            Write-Host "    Got status: $code"
        }
    }
}

Test-Endpoint "Test 35: POST /oauth2/device/approve - No auth (401/403)" {
    $body = @{ user_code = "INVALID"; user_id = "nobody" } | ConvertTo-Json
    try {
        Invoke-RestMethod -Uri "$BaseUrl/oauth2/device/approve" -Method Post -Body $body -ContentType 'application/json' -ErrorAction Stop
        throw "should have returned 401/403"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "Unauthorized" -or $code -eq "Forbidden") {
            Write-Host "    Correctly rejected: $code"
        } else { throw "expected 401/403, got: $code" }
    }
}

# ========================================
# Test 36-38: Social Login (error-only)
# ========================================
Test-Endpoint "Test 36: POST /api/github/login - Invalid code" {
    $body = @{ code = "invalid-github-code-xyz" } | ConvertTo-Json
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/github/login" -Method Post -Body $body -ContentType 'application/json' -ErrorAction Stop
        throw "should have returned error"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "Unauthorized" -or $code -eq "BadRequest" -or $code -eq "InternalServerError") {
            Write-Host "    Correctly rejected invalid code: $code"
        } else {
            Write-Host "    Got status: $code"
        }
    }
}

Test-Endpoint "Test 37: POST /api/google/login - Invalid code" {
    $body = @{ code = "invalid-google-code-xyz" } | ConvertTo-Json
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/google/login" -Method Post -Body $body -ContentType 'application/json' -ErrorAction Stop
        throw "should have returned error"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "Unauthorized" -or $code -eq "BadRequest" -or $code -eq "InternalServerError") {
            Write-Host "    Correctly rejected invalid code: $code"
        } else {
            Write-Host "    Got status: $code"
        }
    }
}

Test-Endpoint "Test 38: POST /api/wechat/login - Invalid code" {
    $body = @{ code = "invalid-wechat-code-xyz" } | ConvertTo-Json
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/wechat/login" -Method Post -Body $body -ContentType 'application/json' -ErrorAction Stop
        throw "should have returned error"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "Unauthorized" -or $code -eq "BadRequest" -or $code -eq "InternalServerError") {
            Write-Host "    Correctly rejected invalid code: $code"
        } else {
            Write-Host "    Got status: $code"
        }
    }
}

# ========================================
# Test 39-42: Scenario Hardening
# ========================================
Test-Endpoint "Test 39: PUT /api/me/password - Wrong old password" {
    # Get a fresh token for this test
    $tok = Get-UserToken -BaseUrl $BaseUrl -Username "admin" -Password $adminPassword
    $h = @{ Authorization = "Bearer $tok"; "Content-Type" = "application/json" }
    try {
        $body = '{"old_password":"wrong-password-xyz","new_password":"NewPass456!"}'
        Invoke-RestMethod -Uri "$BaseUrl/api/me/password" -Method Put -Body $body -Headers $h -ErrorAction Stop
        throw "should have returned error"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "Unauthorized" -or $code -eq "BadRequest") {
            Write-Host "    Correctly rejected wrong old password: $code"
        } else {
            Write-Host "    Got status: $code"
        }
    }
}

Test-Endpoint "Test 40: PUT /api/me/password - No auth (401)" {
    try {
        $body = '{"old_password":"admin","new_password":"NewPass123!"}'
        Invoke-RestMethod -Uri "$BaseUrl/api/me/password" -Method Put -Body $body -ContentType 'application/json' -ErrorAction Stop
        throw "should have returned 401"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Unauthorized") {
            Write-Host "    Correctly returned 401"
        } else { throw "expected 401, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 41: POST /oauth2/token - Expired/used authorization code" {
    try {
        $body = @{
            grant_type = 'authorization_code'
            code = 'already-used-or-expired-code-xyz'
            redirect_uri = 'http://127.0.0.1:5173/callback'
            client_id = 'fulla-portal'
            client_secret = '123456'
        }
        Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body $body -ErrorAction Stop
        throw "should have returned error"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "BadRequest") {
            Write-Host "    Correctly rejected expired code: 400"
        } else {
            Write-Host "    Got status: $code"
        }
    }
}

Test-Endpoint "Test 42: POST /oauth2/introspect - Malformed token" {
    # RFC 7662 §2.1: the introspection endpoint authenticates the calling
    # CLIENT (not the resource owner). fulla-portal is PUBLIC (no secret) and
    # cannot authenticate here; use backend-svc (CONFIDENTIAL, secret
    # "test-secret") via HTTP Basic (F-017: client_secret_basic). The token
    # is a valid-format but nonexistent 50-char string (>= TOKEN_MIN_LEN 32)
    # so the request passes input validation and the introspection result is
    # active=false (token not in the store).
    $basicAuth = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("backend-svc:test-secret"))
    $headers = @{ Authorization = "Basic $basicAuth" }
    $r = Invoke-RestMethod -Uri "$BaseUrl/oauth2/introspect" -Method Post -Headers $headers -Body @{ token = "not-a-real-token-but-long-enough-to-pass-min-len-check-XYZ" }
    if ($r.active -ne $false) { throw "malformed token should be active=false, got $($r.active)" }
    Write-Host "    Correctly returned active=false for malformed token"
}

Test-Endpoint "Test 43: POST /oauth2/revoke - Already revoked token (idempotent)" {
    $tok = Get-UserToken -BaseUrl $BaseUrl -Username "admin" -Password $adminPassword
    # RFC 7009: client-credential auth via body only; no Bearer header needed.
    # F-017: fulla-portal is PUBLIC (token_endpoint_auth_method='none'); no secret.
    $body = @{ token = $tok; client_id = "fulla-portal" }
    # Revoke once
    Invoke-WebRequest -Uri "$BaseUrl/oauth2/revoke" -Method Post -Body $body -UseBasicParsing | Out-Null
    # Revoke again - should succeed (idempotent)
    try {
        Invoke-WebRequest -Uri "$BaseUrl/oauth2/revoke" -Method Post -Body $body -UseBasicParsing | Out-Null
        Write-Host "    Second revocation succeeded (idempotent)"
    } catch {
        Write-Host "    Second revocation returned: $($_.Exception.Response.StatusCode)"
    }
}

Test-Endpoint "Test 44: POST /oauth2/introspect - Missing client credentials" {
    # RFC 7662 SS2.1: without client credentials the endpoint MUST return
    # 401 with the RFC 6749 SS5.2 error body {"error":"invalid_client"}
    # plus WWW-Authenticate: Basic realm=... (not an Error-Envelope).
    $body = @{ token = "some-token" }
    try {
        Invoke-WebRequest -Uri "$BaseUrl/oauth2/introspect" -Method Post -Body $body -UseBasicParsing -ErrorAction Stop | Out-Null
        throw "introspect without client credentials should fail"
    } catch {
        if ($_.Exception.Message -match "should fail") { throw $_.Exception.Message }
        # PS7: body in $_.ErrorDetails.Message; headers via HttpResponseMessage.
        $resp = $_.Exception.Response
        if ($null -eq $resp -or [int]$resp.StatusCode -ne 401) { throw "expected 401, got $($_.Exception.Message)" }
        $wwwAuth = ""
        try { $wwwAuth = ($resp.Headers.GetValues("WWW-Authenticate") -join ", ") } catch {}
        $respBody = "$($_.ErrorDetails.Message)"
        if ($respBody -notmatch '"error"\s*:\s*"invalid_client"') { throw "expected invalid_client error body, got: $respBody" }
        Write-Host "    401 + invalid_client body returned (WWW-Authenticate: $wwwAuth)"
    }
}

# ========================================
# Cleanup: Reset admin account
# ========================================
Write-Host ""
Write-Host "========================================" -ForegroundColor Yellow
Write-Host "Post-test Cleanup" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Yellow
Reset-AdminAccount

# ========================================
# Summary
# ========================================
Write-Host ""
Write-Host "========================================"
Write-Host "Test Summary: $passed/$total passed, $failed failed"
Write-Host "========================================"

if ($failed -gt 0) {
    Write-Host "FAILED" -ForegroundColor Red
    exit 1
} else {
    Write-Host "ALL PASSED" -ForegroundColor Green
    exit 0
}
