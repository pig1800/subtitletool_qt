@echo off
setlocal enabledelayedexpansion

rem -- Initialize MSVC x64 environment --
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo Failed to initialize MSVC environment.
    exit /b 1
)

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo cl.exe not found on PATH after vcvars64.
    exit /b 1
)

rem -- Make Ninja visible (used by the "default" preset) --
set "NINJA_DIR=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if exist "%NINJA_DIR%\ninja.exe" set "PATH=%NINJA_DIR%;%PATH%"

rem -- Ensure Qt6 Multimedia is installed (required by this project) --
rem    A project-local overlay triplet (cmake\triplets\x64-windows-static.cmake)
rem    forces VCPKG_BUILD_TYPE=release. The ffmpeg Debug backend pulled in by
rem    qtmultimedia fails to compile under the current MSVC/UCRT toolchain
rem    (stdlib.h getenv macro clash); this project is Release-only, so the
rem    overlay skips the Debug variant entirely. Same triplet name, so the
rem    "default" CMake preset still matches.
set "VCPKG_ROOT=C:\PIG\vcpkg"
set "OVERLAY_TRIPLETS=%~dp0cmake\triplets"
set "QTMULTIMEDIA_CFG=%VCPKG_ROOT%\installed\x64-windows-static\share\Qt6Multimedia\Qt6MultimediaConfig.cmake"
if not exist "%QTMULTIMEDIA_CFG%" (
    echo Installing Qt6 Multimedia via vcpkg - first time only, this takes a while...
    "%VCPKG_ROOT%\vcpkg.exe" install qtmultimedia:x64-windows-static --overlay-triplets="%OVERLAY_TRIPLETS%"
    if errorlevel 1 (
        echo vcpkg install qtmultimedia failed.
        exit /b 1
    )
)

rem -- Detect a stale or incomplete CMake cache --
rem    Reconfigure when: no cache, the cached compiler is gone, or a prior
rem    configure aborted before writing build.ninja (e.g. a missing dependency
rem    at configure time leaves CMakeCache.txt but no build.ninja).
set "NEED_CONFIGURE=0"
if not exist build\CMakeCache.txt (
    set "NEED_CONFIGURE=1"
) else (
    set "CACHED_CXX="
    for /f "tokens=2 delims==" %%i in ('findstr /b /c:"CMAKE_CXX_COMPILER:FILEPATH=" build\CMakeCache.txt') do set "CACHED_CXX=%%i"
    if not exist "!CACHED_CXX!" set "NEED_CONFIGURE=1"
)
if not exist build\build.ninja set "NEED_CONFIGURE=1"

if "!NEED_CONFIGURE!"=="1" (
    echo Reconfiguring CMake - stale or missing cache...
    if exist build rmdir /s /q build
    cmake --preset default
    if errorlevel 1 (
        echo CMake configure failed.
        exit /b 1
    )
)

rem -- Build --
cmake --build build --config Release --target subtitletool
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Build succeeded.
exit /b 0
