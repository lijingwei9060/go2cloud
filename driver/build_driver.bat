@echo off
echo === go2cloud_flt.sys build ===

set WINSDK=C:\Program Files (x86)\Windows Kits\10
set WDKVER=10.0.28000.0
set WINSDKVER=10.0.26100.0
set KMDFVER=1.35
set VSINSTALL=C:\Program Files\Microsoft Visual Studio\18\Community
set MSVCBIN=%VSINSTALL%\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64
set MSVCINC=%VSINSTALL%\VC\Tools\MSVC\14.51.36231\include

set PATH=%MSVCBIN%;%PATH%

REM WDK kernel headers (10.0.28000.0) + WinSDK shared/ucrt (10.0.26100.0)
set INCLUDE=%WINSDK%\Include\%WDKVER%\km\crt
set INCLUDE=%INCLUDE%;%WINSDK%\Include\%WDKVER%\km
set INCLUDE=%INCLUDE%;%WINSDK%\Include\wdf\kmdf\%KMDFVER%
set INCLUDE=%INCLUDE%;%WINSDK%\Include\%WINSDKVER%\shared
set INCLUDE=%INCLUDE%;%WINSDK%\Include\%WINSDKVER%\ucrt
set INCLUDE=%INCLUDE%;%MSVCINC%
set INCLUDE=%INCLUDE%;..\include

set LIB=%WINSDK%\Lib\%WDKVER%\km\x64
set LIB=%LIB%;%WINSDK%\Lib\wdf\kmdf\x64\%KMDFVER%

cd /d "%~dp0"

REM Clean
if exist obj\ rmdir /s /q obj
mkdir obj

echo [1/2] Compiling...
cl /c /kernel /O2 /GS- /D_AMD64_ /Fo:obj\ driver.c bitmap.c ioctl.c dispatch.c /I..\include
if %ERRORLEVEL% neq 0 (
    echo [FAIL] Compilation failed
    exit /b 1
)
echo [OK] Compilation done

echo [2/2] Linking...
link /DRIVER:WDM /NODEFAULTLIB /ENTRY:DriverEntry /OUT:go2cloud_flt.sys obj\driver.obj obj\bitmap.obj obj\ioctl.obj obj\dispatch.obj WdfDriverEntry.lib WdfLdr.lib ntoskrnl.lib libcntpr.lib BufferOverflowK.lib
if %ERRORLEVEL% neq 0 (
    echo [FAIL] Link failed
    exit /b 1
)
echo [OK] Link done

echo [3/5] Signing .sys...
set SIGNSDK=%WINSDK%\bin\%WINSDKVER%\x64
set CERTFILE=%~dp0go2cloud_test.pfx

if not exist "%CERTFILE%" (
    echo   Creating self-signed test certificate...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$cert = New-SelfSignedCertificate -Subject 'CN=go2cloudTest' -Type CodeSigning -KeyUsage DigitalSignature -CertStoreLocation 'Cert:\CurrentUser\My'; $cert | Export-PfxCertificate -FilePath '%CERTFILE%' -Password (ConvertTo-SecureString -String 'go2cloud' -Force -AsPlainText)"
    if %ERRORLEVEL% neq 0 (
        echo [WARN] Certificate creation failed — signing skipped
        goto :done
    )
)

"%SIGNSDK%\signtool.exe" sign /v /fd sha256 /f "%CERTFILE%" /p go2cloud go2cloud_flt.sys
if %ERRORLEVEL% neq 0 (
    set SIGNERR=%ERRORLEVEL%
    echo [WARN] Signing failed (code %SIGNERR%)^, driver built but NOT signed
    echo   If testsigning is off, use: bcdedit /set testsigning on ^(reboot required^)
    goto :done
)
echo [OK] .sys signed

echo [4/5] Generating catalog...
set INFCAT="%WINSDK%\bin\%WDKVER%\x86\inf2cat.exe"
if not exist %INFCAT% set INFCAT="%WINSDK%\bin\%WDKVER%\x64\inf2cat.exe"
if not exist %INFCAT% (
    echo [WARN] inf2cat.exe not found — catalog skipped, use manual install
    goto :cert_install
)
%INFCAT% /driver:%CD% /os:10_X64,10_NI_X64,10_GE_X64 /verbose
if %ERRORLEVEL% neq 0 (
    echo [WARN] Catalog generation failed
    goto :cert_install
)
echo [OK] Catalog generated

echo   Signing catalog...
"%SIGNSDK%\signtool.exe" sign /v /fd sha256 /f "%CERTFILE%" /p go2cloud go2cloud_flt.cat
if %ERRORLEVEL% neq 0 (
    echo [WARN] Catalog signing failed
) else (
    echo [OK] Catalog signed
)

:cert_install
echo [5/5] Installing test certificate to trusted stores...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Export-Certificate -Cert (Get-AuthenticodeSignature '%CD%\go2cloud_flt.sys').SignerCertificate -FilePath '%CD%\go2cloud_test.cer'" >nul 2>&1
set CERTERR=0
certutil -addstore -f Root go2cloud_test.cer >nul 2>&1
if %ERRORLEVEL% neq 0 set CERTERR=1
certutil -addstore -f TrustedPublisher go2cloud_test.cer >nul 2>&1
if %ERRORLEVEL% neq 0 set CERTERR=1
if %CERTERR% neq 0 (
    echo [WARN] Cert install failed ^(admin required^)^, driver built but may not load
    echo   Run as Administrator and re-run this script, OR manually:
    echo     certutil -addstore -f Root %~dp0go2cloud_test.cer
    echo     certutil -addstore -f TrustedPublisher %~dp0go2cloud_test.cer
) else (
    echo [OK] Cert installed to Root + TrustedPublisher
)

echo [OK] Cert installed

:done
echo Deploying to d:\migrate\cert...
if not exist d:\migrate\cert mkdir d:\migrate\cert
copy /Y go2cloud_flt.sys d:\migrate\cert\ >nul
copy /Y go2cloud_flt.inf d:\migrate\cert\ >nul
if exist go2cloud_flt.cat copy /Y go2cloud_flt.cat d:\migrate\cert\ >nul
echo === go2cloud_flt.sys built ===
echo   Install: pnputil /add-driver d:\migrate\cert\go2cloud_flt.inf /install ^(admin required^)
