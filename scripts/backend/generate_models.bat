@echo off
setlocal

call "%~dp0\env_common.bat"
if errorlevel 1 exit /b 1

REM drogon_ctl generates the ORM models; resolve it from PATH or the
REM Conan cache (shared helper, see ensure_drogon_ctl.bat).
call "%~dp0ensure_drogon_ctl.bat"
if errorlevel 1 exit /b 1

set PROJECT_DIR=%~dp0..\..
REM ORM models live in libs/storage-postgres (M2b Task 18; paths.env rebased
REM in Phase 4 when the old OAuth2Plugin/ directory was deleted).
set MODELS_SRC_DIR=%PROJECT_DIR%\%LIBS_STORAGE_POSTGRES_DIR%\%MODELS_SRC_REL_DIR%
set MODELS_INC_DIR=%PROJECT_DIR%\%LIBS_STORAGE_POSTGRES_DIR%\%MODELS_INC_REL_DIR%
set MODELS_BACKUP=%PROJECT_DIR%\%LIBS_STORAGE_POSTGRES_DIR%\%MODELS_BACKUP_REL_DIR%

echo.
echo ========================================
echo OAuth2 Plugin Model Generation
echo ========================================
echo.

REM Parse arguments
set AUTO_MODE=0
if "%1"=="-y" set AUTO_MODE=1
if "%1"=="--force" set AUTO_MODE=1

if %AUTO_MODE%==0 (
  echo WARNING: This will regenerate ORM models in %MODELS_SRC_DIR%
  pause
)

REM Backup existing models
if exist "%MODELS_SRC_DIR%" (
  echo Backing up existing models to %MODELS_BACKUP%...
  if exist "%MODELS_BACKUP%" rmdir /s /q "%MODELS_BACKUP%"
  mkdir "%MODELS_BACKUP%"
  xcopy /e /i /y "%MODELS_SRC_DIR%" "%MODELS_BACKUP%" >nul
  if exist "%MODELS_INC_DIR%\*.h" xcopy /y "%MODELS_INC_DIR%\*.h" "%MODELS_BACKUP%" >nul
)

echo Generating ORM models...
if not exist "%MODELS_SRC_DIR%" mkdir "%MODELS_SRC_DIR%"

REM drogon_ctl reads model.json FROM the target dir and writes output there.
REM Run from the repo root and pass the models dir directly -- no ../.. depth
REM hack, no dependency on any model.json copy under apps/server.
cd /d "%PROJECT_DIR%"
if %AUTO_MODE%==1 (
  echo y | drogon_ctl create model "%LIBS_STORAGE_POSTGRES_DIR%/%MODELS_SRC_REL_DIR%"
) else (
  drogon_ctl create model "%LIBS_STORAGE_POSTGRES_DIR%/%MODELS_SRC_REL_DIR%"
)

if errorlevel 1 (
  echo [Error] Model generation failed.
  exit /b 1
)

echo Moving header files to %MODELS_INC_DIR%...
if not exist "%MODELS_INC_DIR%" mkdir "%MODELS_INC_DIR%"
REM Remove old headers first to ensure no stale headers remain
del /q "%MODELS_INC_DIR%\*.h" >nul 2>&1
move /y "%MODELS_SRC_DIR%\*.h" "%MODELS_INC_DIR%\" >nul

echo.
echo ========================================
echo Model generation complete!
echo ========================================
endlocal
exit /b 0
