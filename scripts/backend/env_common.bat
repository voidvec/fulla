@echo off
REM Common path validation for all backend scripts.
REM Loads paths.env (repo root) - single source of truth for source/build
REM /SQL/config paths (Task 5 / M0, design.md review H2).
call "%~dp0paths_env.bat"
if errorlevel 1 exit /b 1

if not exist "%~dp0..\..\%FULLA_SERVER_DIR%" (
    echo [Error] Script must be run from 'scripts/backend' directory.
    exit /b 1
)

REM Locate psql: PATH first, then the Windows installer default dirs
REM (the PostgreSQL installer does not add its bin dir to PATH by default).
where psql >nul 2>&1
if not errorlevel 1 goto have_psql
set "PSQL_DIR="
for /d %%D in ("C:\Program Files\PostgreSQL\*") do (
    if exist "%%D\bin\psql.exe" set "PSQL_DIR=%%D\bin"
)
if not defined PSQL_DIR goto psql_missing
echo [Info] psql found in %PSQL_DIR% ^(added to PATH for this script^).
set "PATH=%PSQL_DIR%;%PATH%"
goto have_psql
:psql_missing
echo [Error] psql not found. Install PostgreSQL or add its bin dir ^(e.g. C:\Program Files\PostgreSQL\17\bin^) to PATH.
exit /b 1
:have_psql
exit /b 0
