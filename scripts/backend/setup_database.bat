@echo off
setlocal

call "%~dp0\env_common.bat"
if errorlevel 1 exit /b 1

REM Check for PostgreSQL client
where psql >nul 2>&1
if errorlevel 1 (
    echo [Error] psql not found in PATH.
    exit /b 1
)

set PROJECT_DIR=%~dp0..\..
set MIGRATIONS_DIR=%PROJECT_DIR%\%FULLA_SERVER_DIR%\%SQL_MIGRATIONS_REL_DIR%
set SEED_DIR=%PROJECT_DIR%\%FULLA_SERVER_DIR%\%SQL_SEED_REL_DIR%

REM DB connection settings: honour env vars (the server / CI may override
REM these, e.g. a non-default role or password), falling back to the local
REM dev defaults. Mirrors setup-database.sh.
if not defined FULLA_DB_USER set "FULLA_DB_USER=fulla_user"
if not defined FULLA_DB_NAME set "FULLA_DB_NAME=fulla_db"
if not defined FULLA_DB_PASSWORD set "FULLA_DB_PASSWORD=123456"
if not defined FULLA_DB_HOST set "FULLA_DB_HOST=localhost"
if not defined FULLA_DB_PORT set "FULLA_DB_PORT=5432"

echo Setting up %FULLA_DB_NAME% database ^(role %FULLA_DB_USER%@%FULLA_DB_HOST%:%FULLA_DB_PORT%^)...

set "PGPASSWORD=%FULLA_DB_PASSWORD%"
set PGCLIENTENCODING=UTF8

echo Probing login as "%FULLA_DB_USER%"@%FULLA_DB_HOST%:%FULLA_DB_PORT%...
set "PROBE_ERR=%TEMP%\fulla_probe.err"
psql -U %FULLA_DB_USER% -h %FULLA_DB_HOST% -p %FULLA_DB_PORT% -d postgres -c "SELECT 1;" >nul 2>"%PROBE_ERR%"
if not errorlevel 1 goto probe_ok
echo [Error] Cannot log into PostgreSQL as "%FULLA_DB_USER%"@%FULLA_DB_HOST%:%FULLA_DB_PORT%.
type "%PROBE_ERR%"
echo.
echo NOTE: over TCP, PostgreSQL reports a wrong password and a MISSING ROLE
echo with the same "password authentication failed" error (anti-enumeration).
echo On a fresh install the role is usually missing. Fix (check in order):
echo.
echo  a. role missing - create it once from a superuser shell (the password
echo     below is the FULLA_DB_PASSWORD default/dev value; CREATEDB is what
echo     the script needs):
echo       psql -U postgres -h %FULLA_DB_HOST% -p %FULLA_DB_PORT% -c "CREATE ROLE %FULLA_DB_USER% LOGIN PASSWORD '%FULLA_DB_PASSWORD%' CREATEDB;"
echo     (docker: docker exec ^<pg-container^> psql -U postgres -c "CREATE ROLE %FULLA_DB_USER% LOGIN PASSWORD '...' CREATEDB;")
echo.
echo  b. wrong password - set FULLA_DB_PASSWORD to this role's real password.
echo.
echo  c. server unreachable / connection refused - start PostgreSQL
echo     (Services: postgresql-x64-17) or point FULLA_DB_HOST/FULLA_DB_PORT at it.
echo.
echo Then re-run this script.
if exist "%PROBE_ERR%" del "%PROBE_ERR%"
exit /b 1
:probe_ok
if exist "%PROBE_ERR%" del "%PROBE_ERR%"

echo Dropping existing database...
psql -U %FULLA_DB_USER% -h %FULLA_DB_HOST% -p %FULLA_DB_PORT% -d postgres -c "DROP DATABASE IF EXISTS %FULLA_DB_NAME%;" 2>nul

echo Creating new database...
set "CREATE_ERR=%TEMP%\fulla_create.err"
psql -U %FULLA_DB_USER% -h %FULLA_DB_HOST% -p %FULLA_DB_PORT% -d postgres -c "CREATE DATABASE %FULLA_DB_NAME%;" >nul 2>"%CREATE_ERR%"
if not errorlevel 1 goto create_ok
echo [Error] Failed to create database "%FULLA_DB_NAME%" as role "%FULLA_DB_USER%".
type "%CREATE_ERR%"
echo Match the message above to its fix (run from a superuser shell, then
echo re-run this script):
echo.
echo  1) "database ... already exists" -- the silent DROP step failed,
echo     usually an open connection (running fulla-server, psql, IDE):
echo       psql -U postgres -h %FULLA_DB_HOST% -p %FULLA_DB_PORT% -c "DROP DATABASE %FULLA_DB_NAME% WITH (FORCE);"
echo.
echo  2) "permission denied to create database" -- the role lacks CREATEDB:
echo       psql -U postgres -h %FULLA_DB_HOST% -p %FULLA_DB_PORT% -c "ALTER ROLE %FULLA_DB_USER% CREATEDB;"
if exist "%CREATE_ERR%" del "%CREATE_ERR%"
exit /b 1
:create_ok
if exist "%CREATE_ERR%" del "%CREATE_ERR%"

REM Apply migrations
if exist "%MIGRATIONS_DIR%" (
    echo Applying migrations from %MIGRATIONS_DIR%...
    for %%f in ("%MIGRATIONS_DIR%\V*.sql") do (
        echo   Applying %%~nxf...
        psql -U %FULLA_DB_USER% -h %FULLA_DB_HOST% -p %FULLA_DB_PORT% -d %FULLA_DB_NAME% -f "%%f"
        if errorlevel 1 (
            echo [Error] Failed to apply %%~nxf
            exit /b 1
        )
    )
) else (
    echo [Error] Migrations directory not found: %MIGRATIONS_DIR%
    exit /b 1
)

REM Apply seed data (dev/test only; explicit list - benchmark-only seeds live
REM in benchmarks\fulla\seed and never land in a dev/test database)
if exist "%SEED_DIR%" (
    echo Applying seed data from %SEED_DIR%...
    for %%f in ("dev_admin_user.sql" "dev_admin_console_client.sql" "dev_portal_client.sql" "dev_backend_client.sql") do (
        echo   Applying %%~nxf...
        psql -U %FULLA_DB_USER% -h %FULLA_DB_HOST% -p %FULLA_DB_PORT% -d %FULLA_DB_NAME% -f "%SEED_DIR%\%%~nxf"
        if errorlevel 1 (
            echo [Error] Failed to apply seed %%~nxf
            exit /b 1
        )
    )
)

echo Database setup complete!
endlocal
exit /b 0
