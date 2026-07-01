@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsx86_amd64.bat" > NUL 2>&1
cd /d D:\source\lijingwei9060\go2cloud

del server\*.obj 2>NUL

cl /O2 /utf-8 /Fe:receiver.exe server\main.c server\log.c server\session.c server\block_writer.c server\protocol_decoder.c server\ack.c /Iinclude /Id:\vcpkg\installed\x64-windows\include /link /LIBPATH:d:\vcpkg\installed\x64-windows\lib zstd.lib ws2_32.lib
if %ERRORLEVEL% NEQ 0 goto :error

echo BUILD RECEIVER SUCCESS
goto :end
:error
echo BUILD RECEIVER FAILED
:end
