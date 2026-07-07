@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM make_installer.bat -- builds dist\GentSamplerSetup-<version>.exe from the
REM existing Release artefacts using Inno Setup 6 (ISCC).
REM
REM This script NEVER triggers a build. It only packages what build.bat
REM already produced. Run build.bat first if the artefact dir is stale or
REM missing -- this script fails loudly rather than silently packaging garbage.
REM
REM Flow: parse version from CMakeLists.txt -> pre-flight (3 keepers present,
REM 7 CUDA patterns absent) -> locate ISCC -> compile with /DAppVer=%VER% ->
REM emit dist\GentSamplerSetup-%VER%.exe.
REM ============================================================================

set "SCRIPT_DIR=%~dp0"
set "REPO_ROOT=%SCRIPT_DIR%.."
set "CMAKELISTS=%REPO_ROOT%\CMakeLists.txt"
set "ARTEFACT_DIR=%REPO_ROOT%\build\GentSampler_artefacts\Release\VST3\GentSampler.vst3\Contents\x86_64-win"
set "ISS_FILE=%SCRIPT_DIR%GentSampler.iss"
set "DIST_DIR=%REPO_ROOT%\dist"

echo ============================================================
echo GentSampler installer build
echo ============================================================

REM ---- Step 1: parse VERSION from CMakeLists.txt ---------------------------
if not exist "%CMAKELISTS%" (
    echo [FAIL] CMakeLists.txt not found at "%CMAKELISTS%"
    exit /b 1
)

set "PROJLINE="
for /f "usebackq tokens=*" %%L in (`findstr /r /c:"^project(GentSampler VERSION" "%CMAKELISTS%"`) do (
    set "PROJLINE=%%L"
)

if not defined PROJLINE (
    echo [FAIL] Could not find a "project(GentSampler VERSION ...)" line in CMakeLists.txt
    exit /b 1
)

REM PROJLINE looks like: project(GentSampler VERSION 1.1.0)
REM Strip everything up to and including "VERSION ", then strip the trailing ")"
set "VER="
for /f "tokens=1,2,3 delims=()" %%A in ("!PROJLINE!") do (
    set "INNER=%%B"
)
REM INNER = "GentSampler VERSION 1.1.0"
for /f "tokens=3" %%V in ("!INNER!") do (
    set "VER=%%V"
)

if not defined VER (
    echo [FAIL] Version parse produced an empty result from line: !PROJLINE!
    exit /b 1
)

echo [OK] Parsed version: !VER!

REM ---- Step 2: pre-flight -- 3 keepers present -------------------------------
if not exist "%ARTEFACT_DIR%" (
    echo [FAIL] Staged artefact dir not found: "%ARTEFACT_DIR%" -- run build.bat first.
    exit /b 1
)

set "MISSING="
if not exist "%ARTEFACT_DIR%\GentSampler.vst3" set "MISSING=!MISSING! GentSampler.vst3"
if not exist "%ARTEFACT_DIR%\onnxruntime.dll" set "MISSING=!MISSING! onnxruntime.dll"
if not exist "%ARTEFACT_DIR%\onnxruntime_providers_shared.dll" set "MISSING=!MISSING! onnxruntime_providers_shared.dll"

if defined MISSING (
    echo [FAIL] Missing required keeper file or files: !MISSING!
    exit /b 1
)

echo [OK] All 3 keeper files present.

REM ---- Step 3: pre-flight -- 7 CUDA patterns absent --------------------------
REM NOTE: the 6 wildcard-prefix patterns (cublas*.dll cudart*.dll cudnn*.dll
REM cufft*.dll curand*.dll nvrtc*.dll) are matched via "dir /b" against the
REM staged dir per prefix, NOT via a `for %%P in (cublas*.dll ...)` outer
REM loop -- cmd.exe filesystem-expands wildcard tokens in a for-loop's `in
REM (...)` set against the CURRENT directory before iterating, so any
REM pattern with no match in the CWD silently vanishes from the loop and is
REM never checked. dir /b against the explicit staged-dir path has no such
REM CWD dependency.
set "CUDA_HIT="
for %%P in (cublas cudart cudnn cufft curand nvrtc) do (
    for /f "delims=" %%F in ('dir /b /a-d "%ARTEFACT_DIR%\%%P*.dll" 2^>nul') do (
        set "CUDA_HIT=!CUDA_HIT! %%F"
    )
)
if exist "%ARTEFACT_DIR%\onnxruntime_providers_cuda.dll" set "CUDA_HIT=!CUDA_HIT! onnxruntime_providers_cuda.dll"

if defined CUDA_HIT (
    echo [FAIL] CUDA payload file or files detected in staged artefact dir -- refusing to package: !CUDA_HIT!
    exit /b 1
)

echo [OK] No CUDA payload files present.

REM ---- Step 4: locate ISCC --------------------------------------------------
set "ISCC_EXE="
for /f "usebackq tokens=*" %%I in (`where ISCC 2^>nul`) do (
    if not defined ISCC_EXE set "ISCC_EXE=%%I"
)

if not defined ISCC_EXE (
    if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" (
        set "ISCC_EXE=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
    )
)

if not defined ISCC_EXE (
    if exist "%LocalAppData%\Programs\Inno Setup 6\ISCC.exe" (
        set "ISCC_EXE=%LocalAppData%\Programs\Inno Setup 6\ISCC.exe"
    )
)

if not defined ISCC_EXE (
    echo [FAIL] ISCC.exe not found on PATH or in the standard Inno Setup 6 install locations.
    echo         Install Inno Setup 6 from https://jrsoftware.org/isinfo.php and retry,
    echo         or add ISCC.exe's directory to PATH.
    exit /b 1
)

echo [OK] Using ISCC: "!ISCC_EXE!"

REM ---- Step 5: compile -------------------------------------------------------
if not exist "%ISS_FILE%" (
    echo [FAIL] Installer script not found: "%ISS_FILE%"
    exit /b 1
)

if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"

"!ISCC_EXE!" "/DAppVer=!VER!" "%ISS_FILE%"
if errorlevel 1 (
    echo [FAIL] ISCC compile failed.
    exit /b 1
)

REM ---- Step 6: verify output --------------------------------------------------
set "OUT_EXE=%DIST_DIR%\GentSamplerSetup-!VER!.exe"
if not exist "%OUT_EXE%" (
    echo [FAIL] Expected output not found: "%OUT_EXE%"
    exit /b 1
)

echo ============================================================
echo [OK] Installer built: "%OUT_EXE%"
echo ============================================================

endlocal
exit /b 0
