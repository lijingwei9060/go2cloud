@echo off
setlocal enabledelayedexpansion

REM === MSVC environment ===
set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\18\Community"
set "MSVCBIN=%VSINSTALL%\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64"
set "WINSDK=C:\Program Files (x86)\Windows Kits\10"
set "WINSDKVER=10.0.26100.0"
set "VCPKG=D:\vcpkg\installed\x64-windows"

set "PATH=%MSVCBIN%;%PATH%"

set "INCLUDE=%VSINSTALL%\VC\Tools\MSVC\14.51.36231\include"
set "INCLUDE=%INCLUDE%;%WINSDK%\Include\%WINSDKVER%\ucrt"
set "INCLUDE=%INCLUDE%;%WINSDK%\Include\%WINSDKVER%\um"
set "INCLUDE=%INCLUDE%;%WINSDK%\Include\%WINSDKVER%\shared"
set "INCLUDE=%INCLUDE%;%VCPKG%\include"
set "INCLUDE=%INCLUDE%;D:\source\lijingwei9060\go2cloud"
set "INCLUDE=%INCLUDE%;D:\source\lijingwei9060\go2cloud\client"

set "LIB=%VSINSTALL%\VC\Tools\MSVC\14.51.36231\lib\x64"
set "LIB=%LIB%;%WINSDK%\Lib\%WINSDKVER%\ucrt\x64"
set "LIB=%LIB%;%WINSDK%\Lib\%WINSDKVER%\um\x64"
set "LIB=%LIB%;%VCPKG%\lib"

cd /d D:\source\lijingwei9060\go2cloud

if not exist test\ mkdir test

echo ============================================
echo Building test_protocol.exe
echo ============================================
cl /O2 /utf-8 /Fe:test_protocol.exe test\test_protocol.c client\msgpack.c client\hash.c client\log.c client\queue.c /Iinclude /Iclient /I. /Fotest\ /link /LIBPATH:%VCPKG%\lib
if %ERRORLEVEL% neq 0 (
    echo [FAIL] test_protocol.exe build failed
    exit /b 1
)
echo [OK] test_protocol.exe built

echo.
echo ============================================
echo Running tests...
echo ============================================
D:\source\lijingwei9060\go2cloud\test_protocol.exe
set TESTRC=%ERRORLEVEL%
echo.
echo ============================================
if %TESTRC% equ 0 (
    echo ALL TESTS PASSED
) else (
    echo SOME TESTS FAILED (exit code %TESTRC%)
)
