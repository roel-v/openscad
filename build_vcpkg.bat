@echo off
setlocal enabledelayedexpansion

rem Find vcpkg root first so that we can bail early if it's not found.
if not defined VCPKG_ROOT (
    echo VCPKG_ROOT not set, attempting to locate vcpkg.exe...

    for /f "tokens=*" %%i in ('where vcpkg.exe 2^>nul') do (
        set VCPKG_EXE_PATH=%%i
        goto found
    )

    echo vcpkg.exe not found in PATH.
    goto end

    :found
    for %%i in ("%VCPKG_EXE_PATH%") do (
        set VCPKG_ROOT=%%~dpi
    )
    set VCPKG_ROOT=%VCPKG_ROOT:~0,-1%

    echo Set VCPKG_ROOT to %VCPKG_ROOT%
)

vcpkg install --triplet x64-windows

mkdir build

cmake -B build -S . ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
    -DUSE_BUILTIN_OPENCSG=TRUE ^
    -DENABLE_CAIRO=FALSE ^
    -DCMAKE_EXE_LINKER_FLAGS="/manifest:no" ^
    -DCMAKE_MODULE_LINKER_FLAGS="/manifest:no" ^
    -DCMAKE_SHARED_LINKER_FLAGS="/manifest:no"

cmake --build build --config Debug
cmake --build build --config Release
