@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "CLEANUP_FAILED="

rem Remove retired Bistro data and generated validation/capture output only.
rem This script is intentionally rooted at its own folder (the engine repo).
for %%I in ("%~dp0.") do set "ROOT=%%~fI"

if not exist "%ROOT%\CMakeLists.txt" goto :bad_root
if not exist "%ROOT%\src\main.cpp" goto :bad_root
if not exist "%ROOT%\assets\" goto :bad_root
findstr /I /C:"project(CustomRayTracingEngine" "%ROOT%\CMakeLists.txt" >nul || goto :bad_root

for %%P in (CustomEngine.exe WaterDemo.exe BistroDemo.exe) do (
    tasklist /FI "IMAGENAME eq %%P" 2>nul | find /I "%%P" >nul
    if not errorlevel 1 (
        echo Close %%P before running this cleanup.
        exit /b 2
    )
)

echo.
echo Engine cleanup target: "%ROOT%"
echo.
echo This permanently removes:
echo   - Retired Bistro assets, reference screenshots, and conversion scratch files
echo   - Old isolated DLSS / Sponza / water validation build copies
echo   - Generated captures and Python / CTest caches
echo   - Bistro and test-only executables from build output
echo   - NGX packages for Linux, ARM64, and ARM64EC (this project is Windows x64 only)
echo   - Downloaded DXC and NGX zip archives (their extracted sources are preserved)
echo.
echo It preserves the active Release engine and water executables, active runtime
echo assets except retired Bistro, source tests, README media, checkpoint, vendor
echo code, build\_deps source and headers, and the Windows x64 NVIDIA NGX SDK/runtime.
echo.
choice /C YN /N /M "Proceed with cleanup? [Y/N] "
if errorlevel 2 goto :cancelled

echo.
echo Removing retired Bistro assets and references...
call :RemoveDir "%ROOT%\assets\benchmark\bistro"
call :RemoveDir "%ROOT%\build\Release\assets\benchmark\bistro"
call :RemoveDir "%ROOT%\build\Release\captures"
call :RemoveFile "%ROOT%\build\Release\BistroDemo.exe"
call :RemoveFile "%ROOT%\docs\media\bistro-exterior-dxr-1080p.png"
call :RemoveFile "%ROOT%\docs\media\bistro-interior-viewport.png"
call :RemoveFile "%ROOT%\tools\extract_bistro_asset.py"
call :RemoveFile "%ROOT%\bistro reference 1.png"
call :RemoveFile "%ROOT%\bistro reference 2.png"
call :RemoveFile "%ROOT%\bistro reference 3.png"

echo Removing Bistro scratch files from the generated build folder...
for /D %%D in ("%ROOT%\build\bistro*") do call :RemoveDir "%%~fD"
for %%F in ("%ROOT%\build\bistro*") do if not exist "%%~fF\NUL" call :RemoveFile "%%~fF"
call :RemoveFile "%ROOT%\build\orca-bistro.zip"

echo Removing unused NGX platform packages and download archives...
call :RemoveDir "%ROOT%\build\_deps\ngx-src\lib\Linux_aarch64"
call :RemoveDir "%ROOT%\build\_deps\ngx-src\lib\Linux_x86_64"
call :RemoveDir "%ROOT%\build\_deps\ngx-src\lib\Windows_aarch64"
call :RemoveDir "%ROOT%\build\_deps\ngx-src\lib\Windows_arm64ec"
for %%F in ("%ROOT%\build\_deps\ngx-subbuild\ngx-populate-prefix\src\*.zip") do call :RemoveFile "%%~fF"
for %%F in ("%ROOT%\build\_deps\dxc-subbuild\dxc-populate-prefix\src\*.zip") do call :RemoveFile "%%~fF"

echo Removing old isolated validation build copies...
for /D %%D in ("%ROOT%\build\dlss-*") do call :RemoveDir "%%~fD"
for %%D in (sponza-no-nrd-final sponza-validation-final reflection-validation-final water-validation-final water-reflection-restored-final water-reflection-off-final captures-before-projectid-20261022-033236 dlss-pretest-captures-20261022 jolt_smoke) do call :RemoveDir "%ROOT%\build\%%~D"
for %%D in (build_no_nrd_validation build_no_nrd_validation_vs build_nrd_validation build_validation) do call :RemoveDir "%ROOT%\%%~D"

echo Removing test-only executable outputs and generated caches...
for %%F in (CheckPx.exe TestAssimp.exe TestArchSample.exe MapArch.exe EngineMathTests.exe EngineGltfImportTests.exe DumpTextures.exe) do call :RemoveFile "%ROOT%\build\Release\%%~F"
call :RemoveDir "%ROOT%\Testing"
call :RemoveDir "%ROOT%\__pycache__"
call :RemoveDir "%ROOT%\tools\__pycache__"
call :RemoveFile "%ROOT%\test_desktop.bmp"
call :RemoveFile "%ROOT%\engine_run_log.txt"

echo Clearing generated captures; the README images and GIF are preserved...
call :RemoveDir "%ROOT%\captures"
if not exist "%ROOT%\captures\" mkdir "%ROOT%\captures"

echo.
if defined CLEANUP_FAILED goto :incomplete
echo Cleanup finished. Source code, active Sponza/water runtime files, and
echo required SDK/dependency sources were preserved. Build or test outputs can
echo be regenerated when needed.
exit /b 0

:RemoveDir
if exist "%~1\." (
    echo   Removing directory: "%~1"
    attrib -R "%~1\*" /S /D >nul 2>&1
    rd /S /Q "%~1"
    if exist "%~1\." (
        echo   WARNING: directory remains; check permissions or open files.
        set "CLEANUP_FAILED=1"
    )
)
exit /b

:RemoveFile
if exist "%~1" (
    echo   Removing file: "%~1"
    del /F /Q "%~1"
    if exist "%~1" (
        echo   WARNING: file remains; check permissions or whether it is open.
        set "CLEANUP_FAILED=1"
    )
)
exit /b

:cancelled
echo Cleanup cancelled; no files were removed.
exit /b 0

:bad_root
echo This script must be in the engine repository root beside CMakeLists.txt,
echo src\main.cpp, and assets. No files were removed.
exit /b 1

:incomplete
echo Cleanup finished with warnings. Review the paths reported above and close
echo programs or adjust permissions before rerunning.
exit /b 1
