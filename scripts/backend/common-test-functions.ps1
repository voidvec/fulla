# Common functions for test scripts

# ---------------------------------------------------------------------------
# PKCE (RFC 7636) helpers — required since F-011/RFC 9700 §2.1.1 made PKCE
# mandatory for PUBLIC clients (fulla-portal, fulla-admin-console are both PUBLIC).
# PowerShell port of the bash generate_pkce_verifier/pkce_s256_challenge in
# common-test-functions.sh.
# ---------------------------------------------------------------------------

function ConvertTo-Base64Url {
    param([byte[]]$Bytes)
    return [Convert]::ToBase64String($Bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
}

# Generate a 43-char code_verifier (32 random bytes -> base64url, RFC 7636 §4.1).
function New-PkceVerifier {
    $bytes = New-Object byte[] 32
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    return ConvertTo-Base64Url $bytes
}

# Compute the S256 code_challenge for a verifier (RFC 7636 §4.2).
function New-PkceChallenge {
    param([string]$Verifier)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha256.ComputeHash([Text.Encoding]::ASCII.GetBytes($Verifier))
        return ConvertTo-Base64Url $digest
    } finally {
        $sha256.Dispose()
    }
}

# Generate a verifier + S256 challenge pair. Returns a hashtable with
# 'verifier' and 'challenge' keys (the caller sends challenge on the authorize
# step, verifier on the token exchange).
function New-PkcePair {
    $verifier = New-PkceVerifier
    $challenge = New-PkceChallenge $verifier
    return @{ verifier = $verifier; challenge = $challenge }
}

function Get-PostgresContainer {
    # Try to find THIS project's postgres container (deploy/docker/docker-
    # compose.yml names the service "fulla-postgres"). Matching only
    # "postgres" grabbed ANY postgres container on the machine — e.g. an
    # unrelated "ory-bench-postgres" — and docker-exec'd the admin reset
    # into the WRONG database ("role fulla_user does not exist"), silently
    # breaking every downstream admin test. No match -> fall back to the
    # local-psql branch.
    try {
        $result = docker ps --format "{{.Names}}" 2>$null | Select-String -Pattern "oauth2.*postgres|postgres.*oauth2"
        return $result
    } catch {
        return $null
    }
}

function Invoke-PsqlQuery {
    param(
        [string]$Query,
        [string]$DbUser,
        [string]$DbName,
        [string]$DbPassword,
        [string]$DbHost
    )
    $env:PGPASSWORD = $DbPassword
    psql -U $DbUser -d $DbName -h $DbHost -c $Query 2>$null | Out-Null
    $exitCode = $LASTEXITCODE
    $env:PGPASSWORD = $null
    return $exitCode
}

function Reset-AdminAccount {
    param(
        [string]$DbUser = "fulla_user",
        [string]$DbName = "fulla_db",
        [string]$DbPassword = "123456",
        [string]$DbHost = "localhost",
        [switch]$Silent
    )
    
    if (-not $Silent) {
        Write-Host "Resetting admin account (password + lockout)..." -ForegroundColor Cyan
    }
    
    # Default admin password hash (PBKDF2-SHA256, salt embedded in the hash
    # Password: 'admin'
    $defaultHash = '$pbkdf2-sha256$310000$61646d696e5f736565645f73616c74$6c0307305e1390e1214b15f1f4d0250b2de86aa0e8aa0e008e5cca03084d3d62'
    $defaultSalt = ""
    
    $query = "UPDATE users SET password_hash = '$defaultHash', salt = '$defaultSalt', failed_login_count = 0, locked_until = 0 WHERE username = 'admin';"
    
    try {
        $containerName = Get-PostgresContainer
        if ($containerName) {
            docker exec $containerName psql -U $DbUser -d $DbName -c $query 2>$null | Out-Null
            if (-not $Silent) {
                Write-Host "Admin account reset successfully (Docker)" -ForegroundColor Green
            }
        } else {
            $exitCode = Invoke-PsqlQuery -Query $query -DbUser $DbUser -DbName $DbName -DbPassword $DbPassword -DbHost $DbHost
            if ($exitCode -eq 0) {
                if (-not $Silent) {
                    Write-Host "Admin account reset successfully (Local PostgreSQL)" -ForegroundColor Green
                }
            } else {
                if (-not $Silent) {
                    Write-Host "Warning: Could not reset admin account" -ForegroundColor Yellow
                }
            }
        }
    } catch {
        if (-not $Silent) {
            Write-Host "Warning: Failed to reset admin account: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }
}

function Invoke-ExpectStatus {
    param(
        [scriptblock]$Block,
        [int]$ExpectedStatus
    )
    try {
        & $Block | Out-Null
        throw "expected status $ExpectedStatus but got success"
    } catch {
        if ($_.Exception.Response.StatusCode -eq $ExpectedStatus) {
            return $true
        }
        # Handle numeric status codes
        $statusCode = $_.Exception.Response.StatusCode
        if ($null -ne $statusCode -and [int]$statusCode -eq $ExpectedStatus) {
            return $true
        }
        throw "expected status $ExpectedStatus, got: $statusCode"
    }
}

function Get-UserToken {
    param(
        [string]$BaseUrl,
        [string]$Username,
        [string]$Password
    )
    # F-011/RFC 7636: PKCE mandatory for PUBLIC clients. fulla-portal is PUBLIC
    # ('none' auth method) -> NO client_secret (F-017 rejects a secret from a
    # 'none' client); PKCE code_verifier is the substitute.
    $pkce = New-PkcePair
    $loginBody = @{
        username = $Username; password = $Password
        client_id = 'fulla-portal'
        redirect_uri = 'http://127.0.0.1:5173/callback'
        scope = 'openid profile'; state = "token-$Username-$(Get-Random)"; json = 'true'
        code_challenge = $pkce.challenge; code_challenge_method = 'S256'
    }
    $login = Invoke-RestMethod -Uri "$BaseUrl/oauth2/login" -Method Post -Body $loginBody
    $tok = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body @{
        grant_type = 'authorization_code'; code = $login.code
        redirect_uri = 'http://127.0.0.1:5173/callback'
        client_id = 'fulla-portal'; code_verifier = $pkce.verifier
    }
    return $tok.access_token
}

function Get-AdminToken {
    param(
        [string]$BaseUrl,
        [string]$Username = "admin",
        [string]$Password = "admin"
    )
    # fulla-admin-console is PUBLIC too -> PKCE required, no client_secret.
    $pkce = New-PkcePair
    $loginBody = @{
        username = $Username; password = $Password
        client_id = 'fulla-admin-console'
        redirect_uri = 'http://localhost:5174/admin/callback'
        scope = 'openid profile admin'; state = "adm-$(Get-Random)"; json = 'true'
        code_challenge = $pkce.challenge; code_challenge_method = 'S256'
    }
    $login = Invoke-RestMethod -Uri "$BaseUrl/oauth2/login" -Method Post -Body $loginBody
    $tok = Invoke-RestMethod -Uri "$BaseUrl/oauth2/token" -Method Post -Body @{
        grant_type = 'authorization_code'; code = $login.code
        redirect_uri = 'http://localhost:5174/admin/callback'
        client_id = 'fulla-admin-console'; code_verifier = $pkce.verifier
    }
    return $tok.access_token
}

function Reset-AdminLockout {
    param(
        [string]$DbUser = "fulla_user",
        [string]$DbName = "fulla_db",
        [string]$DbPassword = "123456",
        [string]$DbHost = "localhost",
        [switch]$Silent
    )
    
    if (-not $Silent) {
        Write-Host "Resetting admin account lockout..." -ForegroundColor Cyan
    }
    
    $query = "UPDATE users SET failed_login_count = 0, locked_until = 0 WHERE username = 'admin';"
    
    try {
        $containerName = Get-PostgresContainer
        if ($containerName) {
            docker exec $containerName psql -U $DbUser -d $DbName -c $query 2>$null | Out-Null
            if (-not $Silent) {
                Write-Host "Admin lockout reset successfully (Docker)" -ForegroundColor Green
            }
        } else {
            $exitCode = Invoke-PsqlQuery -Query $query -DbUser $DbUser -DbName $DbName -DbPassword $DbPassword -DbHost $DbHost
            if ($exitCode -eq 0) {
                if (-not $Silent) {
                    Write-Host "Admin lockout reset successfully (Local PostgreSQL)" -ForegroundColor Green
                }
            } else {
                if (-not $Silent) {
                    Write-Host "Warning: Could not reset admin lockout" -ForegroundColor Yellow
                }
            }
        }
    } catch {
        if (-not $Silent) {
            Write-Host "Warning: Failed to reset admin lockout: $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }
}
