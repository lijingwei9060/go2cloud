@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsx86_amd64.bat" > NUL 2>&1
cd /d D:\source\lijingwei9060\go2cloud

del *.obj 2>NUL

cl /c /O2 /utf-8 client\main.c client\log.c client\hash.c client\msgpack.c client\wire.c client\queue.c client\pool.c client\timer.c client\sqlite.c client\volume.c client\block_io.c client\driver_inject.c client\syschk.c /Iinclude /Id:\vcpkg\installed\x64-windows\include
if %ERRORLEVEL% NEQ 0 goto :error

cl /c /O2 /utf-8 /Tpclient\vss.c /Iinclude /Id:\vcpkg\installed\x64-windows\include
if %ERRORLEVEL% NEQ 0 goto :error

link /OUT:client.exe *.obj /LIBPATH:d:\vcpkg\installed\x64-windows\lib zstd.lib sqlite3.lib ole32.lib vssapi.lib ws2_32.lib
if %ERRORLEVEL% NEQ 0 goto :error

copy /Y client.exe d:\migrate\client.exe
echo BUILD+COPY SUCCESS
goto :end
:error
echo BUILD FAILED
:end
