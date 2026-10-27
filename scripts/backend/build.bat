@echo off
setlocal enabledelayedexpansion

call "%~dp0env_setup.bat"
if %errorlevel% neq 0 exit /b 1

echo Checking for running %SERVER_BINARY_NAME% processes...
taskkill /F /IM %SERVER_BINARY_NAME%.exe >nul 2>&1

set "SCRIPT_DIR=%~dp0"
set PROJECT_DIR=%~dp0..\..
set BUILD_TYPE=Release

:parse_args
if "%1"=="" goto end_parse
if /i "%1"=="-debug" (
    set BUILD_TYPE=Debug
    shift
    goto parse_args
)
shift
goto parse_args
:end_parse

REM Map the build configuration to a CMakePresets.json preset name. All
REM builds go through Conan + `cmake --preset`; each preset installs to its
REM own build/<preset-name> directory (see CMakePresets.json binaryDir).
call "%SCRIPT_DIR%resolve_preset.bat" %BUILD_TYPE%
set "PRESET_DIR=%PROJECT_DIR%\%BUILD_DIR%\%CMAKE_PRESET%"

echo Building Project with preset %CMAKE_PRESET% (configuration: %BUILD_TYPE%)

cd /d "%PROJECT_DIR%"

REM CMakeUserPresets.json is a Conan-generated, gitignored artifact whose
REM `include` list points at previously-installed build/<dir>/CMakePresets.json
REM files. A stale include to a now-missing folder makes `cmake --preset`
REM fail to parse. Remove it so `conan install` below regenerates a clean one.
if exist "%PROJECT_DIR%\CMakeUserPresets.json" del /q "%PROJECT_DIR%\CMakeUserPresets.json"

echo Installing dependencies with Conan...
if not exist "%USERPROFILE%\.conan2\profiles\default" (
    echo Initializing default conan profile...
    conan profile detect
)
conan install . -s compiler="msvc" -s compiler.version=194 -s compiler.cppstd=17 -s build_type=%BUILD_TYPE% --output-folder="%BUILD_DIR%\%CMAKE_PRESET%" --build=missing
if %errorlevel% neq 0 (
    echo [Error] Conan install failed!
    exit /b 1
)

REM drogon_create_views() build rules invoke drogon_ctl by bare name,
REM PATH-resolved at build time. Call AFTER conan install above: on a
REM fresh cache the drogon package only exists from this run onward.
call "%SCRIPT_DIR%ensure_drogon_ctl.bat"
if %errorlevel% neq 0 (
    echo [Error] drogon_ctl resolution failed!
    exit /b 1
)

echo Configuring CMake (preset %CMAKE_PRESET%)...
cmake --preset %CMAKE_PRESET%
if %errorlevel% neq 0 (
    echo [Error] CMake configure failed!
    exit /b 1
)

echo Building (preset %CMAKE_PRESET%, config %BUILD_TYPE%)...
cmake --build --preset %CMAKE_PRESET% --config %BUILD_TYPE%
if %errorlevel% neq 0 (
    echo [Error] Build failed!
    exit /b 1
)

echo Copying config files...
REM cmd's copy mishandles the forward slashes used by paths.env values
REM (apps/server, config/config.json): it silently copies 0 files and
REM leaves errorlevel 1. Normalize to backslashes first.
set "CFG_SRC=%PROJECT_DIR%\%FULLA_SERVER_DIR%\%CONFIG_FILE%"
set "CFG_SRC=%CFG_SRC:/=\%"
set "SRV_OUT=%PRESET_DIR%\%SERVER_BUILD_SUBDIR%\%BUILD_TYPE%"
set "SRV_OUT=%SRV_OUT:/=\%"
copy "%CFG_SRC%" "%SRV_OUT%\" /Y
if %errorlevel% neq 0 (
    echo [Error] Failed to copy config to %SRV_OUT%
    exit /b 1
)
REM Phase 5 restructure: tests build under <preset>/tests (was
REM OAuth2Server/test) and their config is a flat config.json there
REM (matches tests/CMakeLists POST_BUILD copy_if_different).
set "TEST_OUT=%PRESET_DIR%\%TESTS_BUILD_SUBDIR%\%BUILD_TYPE%"
set "TEST_OUT=%TEST_OUT:/=\%"
copy "%CFG_SRC%" "%TEST_OUT%\config.json" /Y
if %errorlevel% neq 0 (
    echo [Error] Failed to copy config to tests build dir
    exit /b 1
)

echo Build completed successfully!
endlocal
