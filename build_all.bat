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

set "LIB=%VSINSTALL%\VC\Tools\MSVC\14.51.36231\lib\x64"
set "LIB=%LIB%;%WINSDK%\Lib\%WINSDKVER%\ucrt\x64"
set "LIB=%LIB%;%WINSDK%\Lib\%WINSDKVER%\um\x64"
set "LIB=%LIB%;%VCPKG%\lib"

cd /d D:\source\lijingwei9060\go2cloud

echo ============================================
echo Building receiver.exe
echo ============================================
cl /O2 /utf-8 /Fe:receiver.exe server\*.c /Iinclude /I%VCPKG%\include /link zstd.lib ws2_32.lib /LIBPATH:%VCPKG%\lib
if %ERRORLEVEL% neq 0 (
    echo [FAIL] receiver.exe build failed
    goto :client
)
echo [OK] receiver.exe built

:client
echo.
echo ============================================
echo Building client.exe
echo ============================================
REM vss.c uses COM headers which require C++ compilation
cl /O2 /utf-8 /c /Tpclient\vss.c /Iinclude /I%VCPKG%\include /Fovss.obj
if %ERRORLEVEL% neq 0 (
    echo [FAIL] vss.c compilation failed
    goto :end
)

REM Compile remaining client C files
cl /O2 /utf-8 /Fe:client.exe client\block_io.c client\hash.c client\log.c client\main.c client\msgpack.c client\pool.c client\queue.c client\sqlite.c client\timer.c client\volume.c client\wire.c vss.obj /Iinclude /I%VCPKG%\include /link zstd.lib sqlite3.lib ole32.lib vssapi.lib ws2_32.lib /LIBPATH:%VCPKG%\lib
if %ERRORLEVEL% neq 0 (
    echo [FAIL] client.exe build failed
    goto :end
)
echo [OK] client.exe built

:end
echo.
echo ============================================
echo Copying runtime DLLs...
echo ============================================
if exist "%VCPKG%\bin\zstd.dll"   copy /y "%VCPKG%\bin\zstd.dll"   . >nul && echo [OK] zstd.dll copied
if exist "%VCPKG%\bin\sqlite3.dll" copy /y "%VCPKG%\bin\sqlite3.dll" . >nul && echo [OK] sqlite3.dll copied

echo.
echo ============================================
echo Results:
echo ============================================
if exist receiver.exe (echo   receiver.exe - OK) else (echo   receiver.exe - MISSING)
if exist client.exe   (echo   client.exe   - OK) else (echo   client.exe   - MISSING)
