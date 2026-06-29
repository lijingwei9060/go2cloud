@echo off
REM === Step 1: Setup MSVC environment ===
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if %ERRORLEVEL% neq 0 (
    echo [ERROR] vcvarsall.bat failed with code %ERRORLEVEL%
    exit /b 1
)

REM === Step 2: Build ===
cd /d D:\source\lijingwei9060\go2cloud

echo.
echo === Building receiver.exe ===
cl /O2 /utf-8 /Fe:receiver.exe server\*.c /Iinclude /ID:\vcpkg\installed\x64-windows\include /link libzstd.lib ws2_32.lib /LIBPATH:D:\vcpkg\installed\x64-windows\lib
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Compilation failed with code %ERRORLEVEL%
    exit /b 1
)

echo.
echo === Build result ===
if exist receiver.exe (echo [OK] receiver.exe built successfully) else (echo [FAIL] receiver.exe not found)
dir receiver.exe 2>nul
