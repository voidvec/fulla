@echo off
REM ensure_drogon_ctl.bat - Make drogon_ctl resolvable for this script
REM and its build children; Windows counterpart of ensure_drogon_ctl()
REM in env_common.sh. drogon's drogon_create_views() macro hardcodes
REM the bare "drogon_ctl" name in its build rules, and CMake configure
REM never validates it, so a missing PATH entry only surfaces late as
REM MSB8066 / exit 9009 during the build (CSP view generation).
REM
REM Intentionally NO setlocal: the PATH prepend must outlive this
REM script and apply in the caller's scope (callers run under their own
REM setlocal). Sources, in order: PATH first, then the Conan cache
REM drogon package (drogon/*:with_ctl ships the binary). Both p\bin and
REM p\<pkgid>\bin layouts are globbed -- the cache's internal layout
REM and the drogo<hash> folder name are not stable across cache
REM rebuilds. On a fresh cache nothing is findable until build.bat's
REM conan install has run once.

where drogon_ctl >nul 2>&1
if not errorlevel 1 goto have_drogon_ctl
set "DROGON_BIN="
for /d %%D in ("%USERPROFILE%\.conan2\p\b\drogo*") do (
    if exist "%%D\p\bin\drogon_ctl.exe" set "DROGON_BIN=%%D\p\bin"
    for /d %%P in ("%%D\p\*") do if exist "%%P\bin\drogon_ctl.exe" set "DROGON_BIN=%%P\bin"
)
if not defined DROGON_BIN (
    echo [Error] drogon_ctl not found in PATH or Conan cache.
    echo          Run scripts\backend\build.bat once to install the drogon package via Conan.
    exit /b 1
)
echo [Info] drogon_ctl found in Conan cache: %DROGON_BIN% ^(added to PATH for this script^).
set "PATH=%DROGON_BIN%;%PATH%"
set "DROGON_BIN="
:have_drogon_ctl
exit /b 0
