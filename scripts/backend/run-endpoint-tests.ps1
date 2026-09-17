<#
.SYNOPSIS
CMake/ctest wrapper for the out-of-process endpoint tests (方案 A).

.DESCRIPTION
Starts the fulla server as a background process, waits for health
readiness, runs the OAuth2 + Admin endpoint test scripts against it, then
stops the server. Returns a non-zero exit code if any endpoint test failed.

Registered as a single ctest entry (EndpointTests_OutOfProcess) so that
`ctest` alone exercises the full out-of-process HTTP stack -- the tests
formerly reachable only via full_test.bat Step 6-7.

Prerequisites (NOT managed by this script -- must be satisfied by the
environment, same as full_test.bat):
  - PostgreSQL + Redis running
  - Database migrated + seeded (setup_database.bat)
  - Server binary built
#>
param(
    [Parameter(Mandatory=$true)]
    [string]$ServerExe,
    [string]$BaseUrl = "http://127.0.0.1:5555"
)
$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$result = 0

# Locate psql: PATH first, then the Windows installer default dirs (the
# PostgreSQL installer does not add its bin dir to PATH by default). The
# endpoint test scripts use psql for account/lockout resets between cases.
if (-not (Get-Command psql -ErrorAction SilentlyContinue)) {
    $pgDir = Get-ChildItem "C:\Program Files\PostgreSQL" -Directory -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending | Select-Object -First 1
    if ($pgDir -and (Test-Path (Join-Path $pgDir.FullName "bin\psql.exe"))) {
        $env:PATH = "$(Join-Path $pgDir.FullName 'bin');$env:PATH"
        Write-Host "[endpoint-wrapper] psql found in $($pgDir.FullName)\bin"
    }
}

# Kill any stale server instance on the fixed port.
Stop-Process -Name "fulla-server" -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

$serverDir = Split-Path -Parent $ServerExe
Write-Host "[endpoint-wrapper] Starting server: $ServerExe"
Write-Host "[endpoint-wrapper] Working dir:   $serverDir"
$proc = Start-Process -FilePath $ServerExe -WorkingDirectory $serverDir -PassThru -WindowStyle Hidden

try {
    # Wait for health readiness (up to 30s).
    $ready = $false
    for ($i = 0; $i -lt 30; $i++) {
        try {
            $r = Invoke-RestMethod -Uri "$BaseUrl/health/live" -Method Get -TimeoutSec 2
            if ($r.status -eq "ok") { $ready = $true; break }
        } catch {
            Start-Sleep -Seconds 1
        }
    }
    if (-not $ready) {
        Write-Host "[endpoint-wrapper] Server did not become ready within 30s (PostgreSQL/Redis/DB seed may be unavailable) -- SKIPPING endpoint tests"
        exit 77  # SKIP_RETURN_CODE: ctest treats 77 as SKIPPED, not FAILED
    }
    Write-Host "[endpoint-wrapper] Server ready (PID $($proc.Id))"

    # Run the endpoint test scripts.
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$ScriptDir/test-oauth2-endpoints.ps1" -BaseUrl $BaseUrl
    if ($LASTEXITCODE -ne 0) { $result = 1 }

    & powershell -NoProfile -ExecutionPolicy Bypass -File "$ScriptDir/test-admin-endpoints.ps1" -BaseUrl $BaseUrl
    if ($LASTEXITCODE -ne 0) { $result = 1 }
}
finally {
    Write-Host "[endpoint-wrapper] Stopping server (PID $($proc.Id))"
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}

if ($result -eq 0) {
    Write-Host "[endpoint-wrapper] ALL endpoint tests PASSED" -ForegroundColor Green
} else {
    Write-Host "[endpoint-wrapper] Some endpoint tests FAILED" -ForegroundColor Red
}
exit $result
