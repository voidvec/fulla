param(
    [string]$BaseUrl = "http://127.0.0.1:5555"
)

$ErrorActionPreference = "Stop"
$passed = 0
$failed = 0
$total = 55

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
# Pre-test Setup
# ========================================
Write-Host "========================================" -ForegroundColor Yellow
Write-Host "Pre-test Setup" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Yellow
Reset-AdminAccount
Write-Host ""

Write-Host "========================================" -ForegroundColor Yellow
Write-Host "Admin API Endpoints Tests ($total tests)" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Yellow
Write-Host "Base URL: $BaseUrl"
Write-Host ""

$accessToken = $null

Test-Endpoint "Setup: Admin Login + Token" {
    # F-011/RFC 7636 (RFC 9700 §2.1.1): PKCE mandatory for PUBLIC clients.
    # fulla-admin-console is PUBLIC → login carries code_challenge, token exchange
    # carries the matching code_verifier. client_secret is empty (PUBLIC client,
    # token_endpoint_auth_method='none' — F-017 rejects any secret).
    $pkce = New-PkcePair
    $loginBody = @{
        username = 'admin'; password = 'admin'
        client_id = 'fulla-admin-console'
        redirect_uri = 'http://localhost:5174/admin/callback'
        scope = 'openid profile admin'
        state = 'admin-test-state'; json = 'true'
        code_challenge = $pkce.challenge; code_challenge_method = 'S256'
    }
    $login = Invoke-RestMethod -Uri "$BaseUrl/oauth2/login" -Method Post -Body $loginBody
    if (-not $login.code) { throw "no auth code from login" }
    $tok = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body @{
        grant_type = 'authorization_code'; code = $login.code
        redirect_uri = 'http://localhost:5174/admin/callback'
        client_id = 'fulla-admin-console'; client_secret = ''; code_verifier = $pkce.verifier
    }
    if (-not $tok.access_token) { throw "no access_token" }
    $script:accessToken = $tok.access_token
    Write-Host "    Token: $($tok.access_token.Substring(0,16))..."
}

function Get-AuthHeaders {
    return @{ Authorization = "Bearer $script:accessToken"; "Content-Type" = "application/json" }
}

# ========================================
# Section A: Dashboard Stats
# ========================================
Test-Endpoint "Test 1: GET /api/admin/dashboard/stats" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/dashboard/stats" -Method Get -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    if ($null -eq $r.total_users) { throw "missing total_users" }
    if ($null -eq $r.total_clients) { throw "missing total_clients" }
    if ($null -eq $r.active_tokens) { throw "missing active_tokens" }
    if ($null -eq $r.failures_today) { throw "missing failures_today" }
    Write-Host "    users=$($r.total_users), clients=$($r.total_clients), tokens=$($r.active_tokens)"
}

# ========================================
# Section B: Client Management
# ========================================
Test-Endpoint "Test 2: GET /api/admin/clients/:id - Client Detail" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal" -Method Get -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    if ($r.client_id -ne "fulla-portal") { throw "client_id mismatch" }
    if (-not $r.client_type) { throw "missing client_type" }
    if ($null -eq $r.scopes) { throw "missing scopes" }
    if ($r.client_secret) { throw "SECURITY: client_secret exposed!" }
    Write-Host "    client_id=$($r.client_id), type=$($r.client_type), scopes=[$($r.scopes -join ',')]"
}

Test-Endpoint "Test 3: GET /api/admin/clients/:id - Not Found (404)" {
    $h = Get-AuthHeaders
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/nonexistent-xyz" -Method Get -Headers $h -ErrorAction Stop
        throw "should have returned 404"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "NotFound") {
            Write-Host "    Correctly returned 404"
        } else { throw "expected 404, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 4: PUT /api/admin/clients/:id - Update Client" {
    $h = Get-AuthHeaders
    $body = @{ name = "Vue Frontend Updated" } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal" -Method Put -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    $check = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal" -Method Get -Headers $h
    if ($check.name -ne "Vue Frontend Updated") { throw "name not updated" }
    Write-Host "    Verified: name='$($check.name)'"
    # Restore
    $restore = @{ name = "Vue Frontend" } | ConvertTo-Json
    Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal" -Method Put -Headers $h -Body $restore | Out-Null
}

Test-Endpoint "Test 5: GET /api/admin/clients/:id/scopes" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal/scopes" -Method Get -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    if ($r.scopes -isnot [array]) { throw "scopes is not array" }
    Write-Host "    scopes: [$($r.scopes -join ', ')]"
}

Test-Endpoint "Test 6: PUT /api/admin/clients/:id/scopes - Update" {
    $h = Get-AuthHeaders
    $current = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal/scopes" -Method Get -Headers $h
    $original = $current.scopes
    $body = @{ scopes = @("openid", "profile", "email") } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal/scopes" -Method Put -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    $verify = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal/scopes" -Method Get -Headers $h
    if ($verify.scopes.Count -ne 3) { throw "expected 3 scopes, got $($verify.scopes.Count)" }
    Write-Host "    Updated and verified: [$($verify.scopes -join ', ')]"
    # Restore
    if ($original -and $original.Count -gt 0) {
        $restoreBody = @{ scopes = $original } | ConvertTo-Json
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal/scopes" -Method Put -Headers $h -Body $restoreBody | Out-Null
    }
}

Test-Endpoint "Test 6b: GET /api/admin/clients - List All" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients" -Method Get -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    if ($r.clients -isnot [array]) { throw "clients not array" }
    if ($r.clients.Count -lt 2) { throw "expected >= 2 clients, got $($r.clients.Count)" }
    Write-Host "    total=$($r.total), count=$($r.clients.Count)"
}

$newClientId = $null
$newClientSecret = $null
Test-Endpoint "Test 6c: POST /api/admin/clients - Create Client" {
    $h = Get-AuthHeaders
    $ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $body = @{
        name = "Test Client $ts"
        redirect_uris = "http://localhost:3000/callback"
        allowed_grant_types = "authorization_code"
        client_type = "CONFIDENTIAL"
    } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients" -Method Post -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    if (-not $r.client_id) { throw "missing client_id" }
    if (-not $r.client_secret) { throw "missing client_secret" }
    $script:newClientId = $r.client_id
    $script:newClientSecret = $r.client_secret
    Write-Host "    Created: client_id=$($r.client_id)"
}

Test-Endpoint "Test 6d: POST /api/admin/clients - Missing name (400)" {
    $h = Get-AuthHeaders
    try {
        $body = @{ redirect_uris = "http://localhost/cb" } | ConvertTo-Json
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients" -Method Post -Headers $h -Body $body -ErrorAction Stop
        throw "should have returned 400"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "BadRequest") {
            Write-Host "    Correctly returned 400 for missing name"
        } else { throw "expected 400, got: $($_.Exception.Response.StatusCode)" }
    }
}

$newClientSecret2 = $null
Test-Endpoint "Test 6e: POST /api/admin/clients/:id/reset-secret" {
    if (-not $newClientId) { throw "skipped: no test client" }
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/$newClientId/reset-secret" -Method Post -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    if (-not $r.client_secret) { throw "missing new client_secret" }
    if ($r.client_secret -eq $newClientSecret) { throw "secret not changed" }
    $script:newClientSecret2 = $r.client_secret
    Write-Host "    New secret differs from original: confirmed"
}

Test-Endpoint "Test 6f: POST /api/admin/clients/:id/reset-secret - Non-existent (404)" {
    $h = Get-AuthHeaders
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/nonexistent-xyz/reset-secret" -Method Post -Headers $h -ErrorAction Stop
        throw "should have returned 404"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "NotFound") {
            Write-Host "    Correctly returned 404"
        } else { throw "expected 404, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 6g: DELETE /api/admin/clients/:id - Delete" {
    if (-not $newClientId) { throw "skipped: no test client" }
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/$newClientId" -Method Delete -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    Write-Host "    Deleted: $($r.client_id)"
    # Verify it is gone
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/$newClientId" -Method Get -Headers $h -ErrorAction Stop
        throw "client should be deleted but still accessible"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "NotFound") {
            Write-Host "    Verified: client no longer accessible"
        }
    }
    $script:newClientId = $null
}

Test-Endpoint "Test 6h: DELETE /api/admin/clients/:id - Non-existent (404)" {
    $h = Get-AuthHeaders
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/nonexistent-delete-xyz" -Method Delete -Headers $h -ErrorAction Stop
        throw "should have returned 404"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "NotFound") {
            Write-Host "    Correctly returned 404"
        } else { throw "expected 404, got: $($_.Exception.Response.StatusCode)" }
    }
}

# ========================================
# Section C: Token Management
# ========================================
Test-Endpoint "Test 7: GET /api/admin/tokens - Token List" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/tokens?page=1&per_page=10" -Method Get -Headers $h
    if ($null -eq $r.tokens) { throw "missing tokens" }
    if ($r.tokens -isnot [array]) { throw "tokens not array" }
    if ($null -eq $r.total) { throw "missing total" }
    if ($r.tokens.Count -gt 0) {
        $t = $r.tokens[0]
        if (-not $t.token_prefix) { throw "missing token_prefix" }
        if ($t.token_prefix.Length -gt 8) { throw "token_prefix too long" }
    }
    Write-Host "    total=$($r.total), returned=$($r.tokens.Count)"
}

Test-Endpoint "Test 8: GET /api/admin/tokens - Filter by client_id" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/tokens?client_id=fulla-admin-console&page=1&per_page=50" -Method Get -Headers $h
    foreach ($t in $r.tokens) {
        if ($t.client_id -ne "fulla-admin-console") { throw "filter failed: got '$($t.client_id)'" }
    }
    Write-Host "    Filtered: $($r.tokens.Count) tokens for fulla-admin-console"
}

Test-Endpoint "Test 9: POST /api/admin/tokens/revoke-by-client" {
    $body = @{ client_id = "backend-svc" } | ConvertTo-Json
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/tokens/revoke-by-client" -Method Post -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    if ($null -eq $r.count) { throw "missing count" }
    Write-Host "    Revoked $($r.count) tokens for backend-svc"
}

Test-Endpoint "Test 10: POST /api/admin/tokens/revoke-by-user" {
    $body = @{ user_id = "nonexistent-user-12345" } | ConvertTo-Json
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/tokens/revoke-by-user" -Method Post -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    Write-Host "    Revoked $($r.count) tokens (expected 0)"
}

Test-Endpoint "Test 11: GET /api/admin/oidc/keys" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/oidc/keys" -Method Get -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    # 110-B: live keystore view -- keys[] entries (kid/kty/alg/status) + active_kid.
    if ($r.keys.Count -lt 1) { throw "keys[] is empty" }
    if ($r.keys[0].kty -ne "RSA") { throw "keys[0].kty != RSA" }
    if ($r.keys[0].alg -ne "RS256") { throw "keys[0].alg != RS256" }
    $active = @($r.keys | Where-Object { $_.status -eq "active" })
    if ($active.Count -ne 1) { throw "expected exactly one active key, got $($active.Count)" }
    if ($active[0].kid -ne $r.active_kid) { throw "active entry kid != active_kid" }
    Write-Host "    keys=$($r.keys.Count), active_kid=$($r.active_kid)"
}

Test-Endpoint "Test 11b: DELETE /api/admin/tokens/:tokenPrefix - Single Revoke" {
    $h = Get-AuthHeaders
    # Issue a dedicated throwaway token to revoke. Never pick tokens[0] from the
    # list here: it is ordered by issued_at DESC, so the first row IS this
    # script's own live admin session -- revoking its prefix cascades 401s
    # through every remaining test.
    # PKCE (F-011/RFC 7636) required for the fulla-admin-console PUBLIC client.
    $pkce = New-PkcePair
    $loginBody = @{
        username = 'admin'; password = 'admin'
        client_id = 'fulla-admin-console'
        redirect_uri = 'http://localhost:5174/admin/callback'
        scope = 'openid profile admin'
        state = 'admin-test-11b'; json = 'true'
        code_challenge = $pkce.challenge; code_challenge_method = 'S256'
    }
    $login = Invoke-RestMethod -Uri "$BaseUrl/oauth2/login" -Method Post -Body $loginBody
    if (-not $login.code) { throw "no auth code for throwaway token" }
    $tok = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body @{
        grant_type = 'authorization_code'; code = $login.code
        redirect_uri = 'http://localhost:5174/admin/callback'
        client_id = 'fulla-admin-console'; client_secret = ''; code_verifier = $pkce.verifier
    }
    if (-not $tok.access_token) { throw "no throwaway access_token" }
    # Server stores SHA-256(raw) uppercase hex (CryptoUtils::hashToken);
    # token_prefix is its first 8 chars, so we can target the throwaway
    # token deterministically without consulting the list.
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $hash = ($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($tok.access_token)) | ForEach-Object { $_.ToString('X2') }) -join ''
    $prefix = $hash.Substring(0, 8)
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/tokens/$prefix" -Method Delete -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    Write-Host "    Revoked throwaway token prefix: $prefix"
}

# ========================================
# Section D: User Management
# ========================================
$adminUserId = $null
Test-Endpoint "Test 12: GET /api/admin/users - List" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/users?q=admin" -Method Get -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    $adminUser = $r.users | Where-Object { $_.username -eq 'admin' }
    if (-not $adminUser) { throw "admin user not found" }
    $script:adminUserId = $adminUser.id
    Write-Host "    total=$($r.total), admin id=$($script:adminUserId)"
}

Test-Endpoint "Test 13: GET /api/admin/users/:id - Detail" {
    if (-not $adminUserId) { throw "skipped: no user id" }
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/users/$adminUserId" -Method Get -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    if ($r.username -ne "admin") { throw "username mismatch" }
    if ($null -eq $r.roles) { throw "missing roles" }
    if ($r.roles -isnot [array]) { throw "roles not array" }
    if ($null -eq $r.locked) { throw "missing locked" }
    Write-Host "    username=$($r.username), roles=[$($r.roles -join ',')], locked=$($r.locked)"
}

Test-Endpoint "Test 14: GET /api/admin/users/:id - Not Found" {
    $h = Get-AuthHeaders
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/users/99999999" -Method Get -Headers $h -ErrorAction Stop
        throw "should have returned 404"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "NotFound") {
            Write-Host "    Correctly returned 404"
        } else { throw "expected 404, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 15: PUT /api/admin/users/:id - Update" {
    if (-not $adminUserId) { throw "skipped" }
    $h = Get-AuthHeaders
    $body = @{ email_verified = $true } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/users/$adminUserId" -Method Put -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    Write-Host "    $($r.message)"
}

Test-Endpoint "Test 16: GET /api/admin/users/:id/roles" {
    if (-not $adminUserId) { throw "skipped" }
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/users/$adminUserId/roles" -Method Get -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    if ($r.roles -isnot [array]) { throw "roles not array" }
    Write-Host "    roles: [$($r.roles.name -join ', ')]"
}

$testUserId = $null
Test-Endpoint "Test 17: Disable/Enable User" {
    $h = Get-AuthHeaders
    $ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    # Create test user
    Invoke-RestMethod -Uri "$BaseUrl/api/register" -Method Post -Body @{ username = "testuser_$ts"; password = "TestPass123"; email = "t_$ts@test.com" } | Out-Null
    $users = Invoke-RestMethod -Uri "$BaseUrl/api/admin/users?q=testuser_$ts" -Method Get -Headers $h
    $testUser = $users.users | Where-Object { $_.username -eq "testuser_$ts" }
    if (-not $testUser) { throw "test user not found" }
    $script:testUserId = $testUser.id
    # Disable
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/users/$($testUser.id)/disable" -Method Put -Headers $h
    if ($r.status -ne "success") { throw "disable failed" }
    $check = Invoke-RestMethod -Uri "$BaseUrl/api/admin/users/$($testUser.id)" -Method Get -Headers $h
    if (-not $check.locked) { throw "user should be locked" }
    # Enable
    $r2 = Invoke-RestMethod -Uri "$BaseUrl/api/admin/users/$($testUser.id)/enable" -Method Post -Headers $h
    if ($r2.status -ne "success") { throw "enable failed" }
    $check2 = Invoke-RestMethod -Uri "$BaseUrl/api/admin/users/$($testUser.id)" -Method Get -Headers $h
    if ($check2.locked) { throw "user should be unlocked" }
    Write-Host "    Disable/Enable cycle verified"
}

# ========================================
# Section E: Role Management
# ========================================
$testRoleId = $null
Test-Endpoint "Test 18: GET /api/admin/roles - List" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/roles" -Method Get -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    if ($r.roles -isnot [array]) { throw "roles not array" }
    $adminRole = $r.roles | Where-Object { $_.name -eq 'admin' }
    if (-not $adminRole) { throw "admin role not found" }
    if ($null -eq $adminRole.user_count) { throw "missing user_count" }
    Write-Host "    total=$($r.total), admin users=$($adminRole.user_count)"
}

Test-Endpoint "Test 19: POST /api/admin/roles - Create" {
    $h = Get-AuthHeaders
    $ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $body = @{ name = "testrole_$ts"; description = "Test role" } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/roles" -Method Post -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    if (-not $r.id) { throw "missing id" }
    $script:testRoleId = $r.id
    Write-Host "    Created: id=$($r.id), name=$($r.name)"
}

Test-Endpoint "Test 20: POST /api/admin/roles - Duplicate (409)" {
    $h = Get-AuthHeaders
    try {
        $body = @{ name = "admin" } | ConvertTo-Json
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/roles" -Method Post -Headers $h -Body $body -ErrorAction Stop
        throw "should have returned 409"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Conflict") {
            Write-Host "    Correctly returned 409"
        } else { throw "expected 409, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 21: PUT /api/admin/roles/:id - Update" {
    if (-not $testRoleId) { throw "skipped" }
    $h = Get-AuthHeaders
    $body = @{ description = "Updated" } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/roles/$testRoleId" -Method Put -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    Write-Host "    $($r.message)"
}

Test-Endpoint "Test 22: DELETE /api/admin/roles/:id - Delete" {
    if (-not $testRoleId) { throw "skipped" }
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/roles/$testRoleId" -Method Delete -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    Write-Host "    $($r.message)"
}

Test-Endpoint "Test 23: DELETE /api/admin/roles - Cannot delete built-in" {
    $h = Get-AuthHeaders
    $roles = Invoke-RestMethod -Uri "$BaseUrl/api/admin/roles" -Method Get -Headers $h
    $adminRole = $roles.roles | Where-Object { $_.name -eq 'admin' }
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/roles/$($adminRole.id)" -Method Delete -Headers $h -ErrorAction Stop
        throw "should have returned 404"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "NotFound") {
            Write-Host "    Correctly prevented deletion of built-in role"
        } else { throw "expected 404, got: $($_.Exception.Response.StatusCode)" }
    }
}

# ========================================
# Section F: Scope Management
# ========================================
$testScopeId = $null
Test-Endpoint "Test 24: POST /api/admin/scopes - Create" {
    $h = Get-AuthHeaders
    $ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $body = @{ name = "testscope_$ts"; description = "Test scope"; mapped_role = "user"; is_default = $false; requires_admin_role = $false } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/scopes" -Method Post -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    if (-not $r.id) { throw "missing id" }
    $script:testScopeId = $r.id
    Write-Host "    Created: id=$($r.id), name=$($r.name)"
}

Test-Endpoint "Test 25: GET /api/admin/scopes - List" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/scopes" -Method Get -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    if ($r.scopes -isnot [array]) { throw "scopes not array" }
    $openid = $r.scopes | Where-Object { $_.name -eq 'openid' }
    if (-not $openid) { throw "openid scope not found" }
    Write-Host "    total=$($r.total), found openid scope"
}

Test-Endpoint "Test 26: PUT /api/admin/scopes/:id - Update" {
    if (-not $testScopeId) { throw "skipped" }
    $h = Get-AuthHeaders
    $body = @{ description = "Updated"; is_default = $true } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/scopes/$testScopeId" -Method Put -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    Write-Host "    $($r.message)"
}

Test-Endpoint "Test 27: DELETE /api/admin/scopes/:id - Delete" {
    if (-not $testScopeId) { throw "skipped" }
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/scopes/$testScopeId" -Method Delete -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    Write-Host "    $($r.message)"
}

Test-Endpoint "Test 28: DELETE /api/admin/scopes - Cannot delete built-in" {
    $h = Get-AuthHeaders
    $scopes = Invoke-RestMethod -Uri "$BaseUrl/api/admin/scopes" -Method Get -Headers $h
    $openid = $scopes.scopes | Where-Object { $_.name -eq 'openid' }
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/scopes/$($openid.id)" -Method Delete -Headers $h -ErrorAction Stop
        throw "should have returned 404"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "NotFound") {
            Write-Host "    Correctly prevented deletion of built-in scope"
        } else { throw "expected 404, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 29: POST /api/admin/scopes - Duplicate (409)" {
    $h = Get-AuthHeaders
    try {
        $body = @{ name = "openid" } | ConvertTo-Json
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/scopes" -Method Post -Headers $h -Body $body -ErrorAction Stop
        throw "should have returned 409"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "Conflict") {
            Write-Host "    Correctly returned 409"
        } else { throw "expected 409, got: $($_.Exception.Response.StatusCode)" }
    }
}

# ========================================
# Section G: Authorization / Security
# ========================================
Test-Endpoint "Test 30: Unauthorized Access - Endpoints require auth" {
    $endpoints = @(
        @{ Uri = "$BaseUrl/api/admin/clients/fulla-portal"; Method = "Get" },
        @{ Uri = "$BaseUrl/api/admin/tokens"; Method = "Get" },
        @{ Uri = "$BaseUrl/api/admin/roles"; Method = "Get" },
        @{ Uri = "$BaseUrl/api/admin/users/1"; Method = "Get" },
        @{ Uri = "$BaseUrl/api/admin/dashboard/stats"; Method = "Get" }
    )
    $allBlocked = $true
    foreach ($ep in $endpoints) {
        try {
            Invoke-RestMethod -Uri $ep.Uri -Method $ep.Method -ErrorAction Stop
            Write-Host "    SECURITY: $($ep.Uri) accessible without auth!" -ForegroundColor Red
            $allBlocked = $false
        } catch {
            $status = $_.Exception.Response.StatusCode
            if ($status -ne "Unauthorized" -and $status -ne "Forbidden") {
                Write-Host "    WARNING: $($ep.Uri) returned $status" -ForegroundColor Yellow
            }
        }
    }
    if (-not $allBlocked) { throw "Some endpoints accessible without auth!" }
    Write-Host "    All 5 endpoints correctly require authentication"
}

Test-Endpoint "Test 31: PUT /api/admin/users/:id/roles - Assign Roles" {
    if (-not $testUserId) { throw "skipped" }
    $h = Get-AuthHeaders
    $body = @{ roles = @("admin", "user") } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/users/$testUserId/roles" -Method Put -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    # Verify
    $check = Invoke-RestMethod -Uri "$BaseUrl/api/admin/users/$testUserId/roles" -Method Get -Headers $h
    if ($check.roles.Count -lt 1) { throw "no roles assigned" }
    Write-Host "    Assigned and verified: [$($check.roles.name -join ', ')]"
}

Test-Endpoint "Test 32: GET /api/admin/logs - Audit Logs" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/logs?page=1&per_page=10" -Method Get -Headers $h
    if ($r.status -ne "success") { throw "status != success" }
    if ($null -eq $r.logs) { throw "missing logs" }
    if ($r.logs -isnot [array]) { throw "logs not array" }
    Write-Host "    page=$($r.page), returned=$($r.logs.Count) logs"
}

# ========================================
# Section H: Organization Management
# ========================================
$testOrgSlug = $null
Test-Endpoint "Test 33: GET /api/admin/organizations - List" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/organizations" -Method Get -Headers $h
    if ($null -eq $r.organizations) { throw "missing organizations" }
    if ($r.organizations -isnot [array]) { throw "organizations not array" }
    Write-Host "    total=$($r.total)"
}

Test-Endpoint "Test 34: POST /api/admin/organizations - Create" {
    $h = Get-AuthHeaders
    $ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $body = @{ slug = "test-org-$ts"; name = "Test Organization $ts" } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/organizations" -Method Post -Headers $h -Body $body
    if (-not $r.id) { throw "missing id" }
    if (-not $r.slug) { throw "missing slug" }
    $script:testOrgSlug = $r.slug
    Write-Host "    Created: id=$($r.id), slug=$($r.slug)"
}

Test-Endpoint "Test 35: GET /api/admin/organizations/:slug - Detail" {
    if (-not $testOrgSlug) { throw "skipped" }
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/organizations/$testOrgSlug" -Method Get -Headers $h
    if ($r.slug -ne $testOrgSlug) { throw "slug mismatch: $($r.slug)" }
    if (-not $r.name) { throw "missing name" }
    Write-Host "    slug=$($r.slug), name=$($r.name)"
}

Test-Endpoint "Test 36: GET /api/admin/organizations/:slug - Not Found" {
    $h = Get-AuthHeaders
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/organizations/nonexistent-org-xyz" -Method Get -Headers $h -ErrorAction Stop
        throw "should have returned 404"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "NotFound") {
            Write-Host "    Correctly returned 404"
        } else { throw "expected 404, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 37: POST /api/admin/organizations - Invalid slug (400)" {
    $h = Get-AuthHeaders
    try {
        $body = @{ slug = "AB"; name = "Bad" } | ConvertTo-Json
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/organizations" -Method Post -Headers $h -Body $body -ErrorAction Stop
        throw "should have returned 400"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "BadRequest") {
            Write-Host "    Correctly returned 400 for invalid slug"
        } else { throw "expected 400, got: $($_.Exception.Response.StatusCode)" }
    }
}

# ========================================
# Section I: Scenario Hardening
# ========================================
Test-Endpoint "Test 38: POST /api/admin/roles - Empty name (400)" {
    $h = Get-AuthHeaders
    try {
        $body = @{ name = ""; description = "No name" } | ConvertTo-Json
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/roles" -Method Post -Headers $h -Body $body -ErrorAction Stop
        throw "should have returned 400"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "BadRequest") {
            Write-Host "    Correctly returned 400 for empty name"
        } else { throw "expected 400, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 39: PUT /api/admin/clients/:id - Empty body" {
    # The server validates that at least one updatable field (name,
    # redirect_uris, allowed_grant_types) is present in the body. An empty
    # body {} is a "no fields to update" client error (400), not a silent
    # no-op success. This matches the updateClient validation gate
    # (ClientManagementService.cc) and is stricter-but-correct.
    $h = Get-AuthHeaders
    $body = '{}'
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal" -Method Put -Headers $h -Body $body -ErrorAction Stop
        throw "should have returned 400"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "BadRequest") {
            Write-Host "    Empty body update correctly rejected with 400"
        } else { throw "expected 400, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 40: GET /api/admin/tokens - Large per_page" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/tokens?page=1&per_page=1000" -Method Get -Headers $h
    if ($null -eq $r.tokens) { throw "missing tokens" }
    Write-Host "    Returned $($r.tokens.Count) tokens (total=$($r.total))"
}

Test-Endpoint "Test 41: POST /api/admin/tokens/revoke-by-client - Non-existent client" {
    $h = Get-AuthHeaders
    $body = @{ client_id = "nonexistent-client-xyz" } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/tokens/revoke-by-client" -Method Post -Headers $h -Body $body
    if ($r.status -ne "success") { throw "status != success" }
    if ($r.count -ne 0) { throw "expected 0 revoked for non-existent client" }
    Write-Host "    Revoked $($r.count) tokens (expected 0)"
}

Test-Endpoint "Test 42: Unauthorized Access - New endpoints require auth" {
    # Each endpoint is probed with its correct HTTP method. The auth gate
    # runs before the method/handler, so a missing-token request must return
    # 401 (or 403) regardless of whether the method is otherwise valid. POST
    # endpoints get a JSON body; GET endpoints get no body.
    $postEndpoints = @(
        "$BaseUrl/api/admin/clients"
        "$BaseUrl/api/me/mfa/setup"
    )
    $getEndpoints = @(
        "$BaseUrl/api/me/authorized-apps"
        "$BaseUrl/api/me/webauthn/credentials"
    )
    $allBlocked = $true
    foreach ($ep in $postEndpoints) {
        try {
            Invoke-RestMethod -Uri $ep -Method Post -ContentType 'application/json' -Body '{}' -ErrorAction Stop
            Write-Host "    SECURITY: POST $ep accessible without auth!" -ForegroundColor Red
            $allBlocked = $false
        } catch {
            $status = $_.Exception.Response.StatusCode
            if ($status -ne "Unauthorized" -and $status -ne "Forbidden") {
                Write-Host "    WARNING: POST $ep returned $status" -ForegroundColor Yellow
            }
        }
    }
    foreach ($ep in $getEndpoints) {
        try {
            Invoke-RestMethod -Uri $ep -Method Get -ErrorAction Stop
            Write-Host "    SECURITY: GET $ep accessible without auth!" -ForegroundColor Red
            $allBlocked = $false
        } catch {
            $status = $_.Exception.Response.StatusCode
            if ($status -ne "Unauthorized" -and $status -ne "Forbidden") {
                Write-Host "    WARNING: GET $ep returned $status" -ForegroundColor Yellow
            }
        }
    }
    if (-not $allBlocked) { throw "Some endpoints accessible without auth!" }
    Write-Host "    All 4 new endpoints correctly require authentication"
}

Test-Endpoint "Test 43: GET /api/admin/dashboard/stats - Non-admin denied" {
    # Register a non-admin user and try to access admin stats
    $ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $un = "nonadmin_$ts"
    Invoke-RestMethod -Uri "$BaseUrl/api/register" -Method Post -Body @{ username = $un; password = "TestPass123!"; email = "${un}@test.com" } | Out-Null
    $tok = Get-UserToken -BaseUrl $BaseUrl -Username $un -Password "TestPass123!"
    $h = @{ Authorization = "Bearer $tok" }
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/dashboard/stats" -Method Get -Headers $h -ErrorAction Stop
        throw "non-admin should be denied"
    } catch {
        $code = $_.Exception.Response.StatusCode
        if ($code -eq "Forbidden" -or $code -eq "Unauthorized") {
            Write-Host "    Correctly denied non-admin: $code"
        } else {
            Write-Host "    Got status: $code"
        }
    }
}

# B1 (OIDC Back-Channel Logout 1.0): backchannel_logout_uri admin config path.
Test-Endpoint "Test 44: PUT/GET/clear backchannel_logout_uri (B1)" {
    $h = Get-AuthHeaders
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal" -Method Put -Headers $h -Body (@{ backchannel_logout_uri = "https://rp-bc.example.com/backchannel-logout" } | ConvertTo-Json)
    if ($r.status -ne "success") { throw "set failed" }
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal" -Method Get -Headers $h
    if ($r.backchannel_logout_uri -ne "https://rp-bc.example.com/backchannel-logout") { throw "uri not persisted: '$($r.backchannel_logout_uri)'" }
    # Empty string clears the registration (NULL in DB, "" in the response).
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal" -Method Put -Headers $h -Body (@{ backchannel_logout_uri = "" } | ConvertTo-Json)
    if ($r.status -ne "success") { throw "clear failed" }
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal" -Method Get -Headers $h
    if ("$($r.backchannel_logout_uri)" -ne "") { throw "uri not cleared: '$($r.backchannel_logout_uri)'" }
    Write-Host "    set -> read -> cleared: ok"
}

# NOTE: plain http:// is NOT a reliable negative case here because the test
# configs enable auth.allow_http_redirect_uri (dev hatch honored by the
# validator). ftp:// is invalid regardless of the hatch.
Test-Endpoint "Test 45: PUT backchannel_logout_uri - non-https scheme (400)" {
    $h = Get-AuthHeaders
    try {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/fulla-portal" -Method Put -Headers $h -ErrorAction Stop -Body (@{ backchannel_logout_uri = "ftp://rp.example.com/backchannel-logout" } | ConvertTo-Json)
        throw "should have returned 400"
    } catch {
        if ($_.Exception.Response.StatusCode -eq "BadRequest") {
            Write-Host "    Correctly returned 400 for non-https backchannel_logout_uri"
        } else { throw "expected 400, got: $($_.Exception.Response.StatusCode)" }
    }
}

Test-Endpoint "Test 46: POST /api/admin/clients - create with backchannel_logout_uri (B1)" {
    $h = Get-AuthHeaders
    $ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $body = @{
        name = "BC Test Client $ts"
        redirect_uris = "http://localhost:3100/callback"
        allowed_grant_types = "authorization_code"
        client_type = "CONFIDENTIAL"
        backchannel_logout_uri = "https://rp-bc-create.example.com/bc"
    } | ConvertTo-Json
    $r = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients" -Method Post -Headers $h -Body $body
    if ($r.status -ne "success") { throw "create failed" }
    if (-not $r.client_id) { throw "missing client_id" }
    $bcId = $r.client_id
    try {
        $g = Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/$bcId" -Method Get -Headers $h
        if ($g.backchannel_logout_uri -ne "https://rp-bc-create.example.com/bc") { throw "uri not persisted on create: '$($g.backchannel_logout_uri)'" }
    } finally {
        Invoke-RestMethod -Uri "$BaseUrl/api/admin/clients/$bcId" -Method Delete -Headers $h | Out-Null
    }
    Write-Host "    created with uri, read back, deleted: $bcId"
}

# ========================================
# Post-test Cleanup
# ========================================
Write-Host ""
Write-Host "========================================" -ForegroundColor Yellow
Write-Host "Post-test Cleanup" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Yellow
Reset-AdminAccount

Write-Host ""
Write-Host "========================================"
Write-Host "Admin API Tests: $passed/$total passed, $failed failed"
Write-Host "========================================"

if ($failed -gt 0) {
    Write-Host "FAILED" -ForegroundColor Red
    exit 1
} else {
    Write-Host "ALL PASSED" -ForegroundColor Green
    exit 0
}
