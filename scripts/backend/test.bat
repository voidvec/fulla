@echo off
setlocal enabledelayedexpansion

call "%~dp0\env_common.bat"
if errorlevel 1 exit /b 1

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%~dp0..\.."
set BUILD_TYPE=Release
REM Default to minimal output: only failed tests print their log, plus the
REM final pass/fail/total summary line. Pass -q to suppress even the
REM per-test START/PASS lines (-Q = summary only).
set VERBOSE=--output-on-failure

:parse_args
if "%1"=="" goto end_parse
if /i "%1"=="-debug" (
    set BUILD_TYPE=Debug
    shift
    goto parse_args
)
if /i "%1"=="-release" (
    set BUILD_TYPE=Release
    shift
    goto parse_args
)
if /i "%1"=="-q" (
    set VERBOSE=-Q
    shift
    goto parse_args
)
shift
goto parse_args
:end_parse

REM Map the build configuration to its Conan-installed CMakePresets.json
REM preset directory (build/<preset>), matching what build.bat produced.
call "%SCRIPT_DIR%resolve_preset.bat" %BUILD_TYPE%
set "PRESET_DIR=%PROJECT_DIR%\%BUILD_DIR%\%CMAKE_PRESET%"

echo ========================================
echo Running OAuth2 Tests (Dual-Config)
echo ========================================
echo Build Type: %BUILD_TYPE%

if not exist "%PRESET_DIR%" (
    echo [Error] Build directory not found: %PRESET_DIR%. Please run build.bat first.
    exit /b 1
)

REM ctest spawns builds too: SdkSmoke.FullStack compiles the
REM full-stack-host example, whose CSP view custom command invokes
REM drogon_ctl by bare name (PATH-resolved at build time). build.bat's
REM PATH prepend does not cross script boundaries, so resolve it here
REM as well.
call "%SCRIPT_DIR%ensure_drogon_ctl.bat"
if errorlevel 1 exit /b 1

REM Phase 5 restructure: tests build under <preset>/tests (was
REM OAuth2Server/test); their runtime config is a flat config.json there
REM (tests/CMakeLists POST_BUILD), NOT the source-tree config/ subpath.
set "TEST_WORK_DIR=%PRESET_DIR%\%TESTS_BUILD_SUBDIR%\%BUILD_TYPE%"
set "TEST_CONFIG=%TEST_WORK_DIR%\config.json"

cd /d "%PRESET_DIR%"

REM --- Run 1: Standard config.json ---
echo.
echo [1/2] Running tests with standard %CONFIG_FILE%...
REM The JUnit report feeds the full_test endpoint dedup (#119): full_test.bat
REM skips its manual endpoint re-run when EndpointTests_OutOfProcess is proven
REM green in this run.
ctest -C %BUILD_TYPE% %VERBOSE% --output-junit "%PRESET_DIR%\Testing\junit-config-standard.xml"
if !errorlevel! neq 0 (
    echo [FAIL] Tests failed with standard %CONFIG_FILE%
    exit /b 1
)
echo [PASS] Standard config tests successful.

REM --- Run 2: config.ci.json ---
echo.
echo [2/2] Running tests with %CONFIG_CI_FILE%...
if not exist "%PROJECT_DIR%\%FULLA_SERVER_DIR%\%CONFIG_CI_FILE%" (
    echo [SKIP] %CONFIG_CI_FILE% not found, skipping second run.
    goto done
)

if not exist "%TEST_WORK_DIR%" (
    echo [Error] Test work dir not found: %TEST_WORK_DIR%
    exit /b 1
)

REM Backup original and use CI config
REM cmd's copy mishandles forward slashes from paths.env values; normalize.
set "CI_CFG_SRC=%PROJECT_DIR%\%FULLA_SERVER_DIR%\%CONFIG_CI_FILE%"
set "CI_CFG_SRC=!CI_CFG_SRC:/=\!"
copy /Y "%TEST_CONFIG%" "%TEST_CONFIG%.bak" >nul
copy /Y "!CI_CFG_SRC!" "%TEST_CONFIG%" >nul

ctest -C %BUILD_TYPE% %VERBOSE% --output-junit "%PRESET_DIR%\Testing\junit-config-ci.xml"
set "CI_EXIT=!errorlevel!"

REM Restore original config immediately
copy /Y "%TEST_CONFIG%.bak" "%TEST_CONFIG%" >nul
del "%TEST_CONFIG%.bak" >nul 2>&1

if !CI_EXIT! neq 0 (
    echo [FAIL] Tests failed with %CONFIG_CI_FILE%
    exit /b 1
)
echo [PASS] CI config tests successful.

:done
echo.
echo ========================================
echo All test runs completed successfully
echo ========================================

endlocal
exit /b 0
