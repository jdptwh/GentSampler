@echo off
setlocal
cd /d "%~dp0"

echo ============================================
echo   GentSampler - one-click build
echo ============================================
echo.

where cmake >nul 2>nul
if errorlevel 1 (
    echo [!] CMake not found on PATH. See README.md step 1, then reopen this window.
    pause
    exit /b 1
)
where git >nul 2>nul
if errorlevel 1 (
    echo [!] Git not found on PATH. See README.md step 1, then reopen this window.
    pause
    exit /b 1
)

echo === Configuring - first run downloads JUCE, takes a few minutes ===
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo.
    echo [!] Configure failed. Make sure Visual Studio 2022 Build Tools with the
    echo     C++ workload is installed - see README.md step 1.
    pause
    exit /b 1
)

echo.
echo === Building Release ===
cmake --build build --config Release --parallel
if errorlevel 1 (
    echo.
    echo [!] Build failed. Copy the error text above and paste it back to Claude for a fix.
    pause
    exit /b 1
)

set "SRC=build\GentSampler_artefacts\Release\VST3\GentSampler.vst3"

echo.
echo === Installing plugin ===
xcopy /e /i /y "%SRC%" "%CommonProgramFiles%\VST3\GentSampler.vst3\" >nul 2>nul
if errorlevel 1 (
    echo [i] No admin rights for Common Files - installing to your Documents instead.
    xcopy /e /i /y "%SRC%" "%USERPROFILE%\Documents\VST3\GentSampler.vst3\" >nul
    echo.
    echo [OK] Installed to: %USERPROFILE%\Documents\VST3
    echo      In FL Studio: Options, File settings, add that folder as a plugin search path.
) else (
    echo [OK] Installed to: %CommonProgramFiles%\VST3
)

echo.
echo Done! In FL Studio: Options, Manage plugins, Find more plugins.
echo GentSampler will show up under Installed, Generators, VST3.
pause
