@echo off
rem Pack the release zip RecompLauncher plays from: the asInvoker installer (ttt2_setup_probe.exe) and
rem port\launcher\recomplauncher.json, side by side at the top of the zip. See port\LAUNCHER.md.
rem Run tools\build_setup.cmd first. Upload the result next to ttt2_setup.exe on the release.
rem Usage: package_launcher_zip.cmd [build-directory]
rem Example: package_launcher_zip.cmd build-o1   (writes dist\ttt2-0.2.0-windows-amd64.zip)
setlocal
set "PORT_BUILD_DIR=%~1"
if not defined PORT_BUILD_DIR set "PORT_BUILD_DIR=build-o1"
set "ROOT=%~dp0.."
set "PROBE=%ROOT%\port\%PORT_BUILD_DIR%\setup\ttt2_setup_probe.exe"
set "MANIFEST=%ROOT%\port\launcher\recomplauncher.json"
if not exist "%PROBE%" (
    echo Missing %PROBE%. Run tools\build_setup.cmd first.
    exit /b 1
)
rem The version is the one the installer carries: TTT2_VERSION in port\CMakeLists.txt.
for /f tokens^=2^ delims^=^" %%v in ('findstr /b /c:"set(TTT2_VERSION " "%ROOT%\port\CMakeLists.txt"') do set "VERSION=%%v"
if not defined VERSION (
    echo Could not read TTT2_VERSION from port\CMakeLists.txt.
    exit /b 1
)
set "STAGE=%ROOT%\dist\launcher-stage"
set "ZIP=%ROOT%\dist\ttt2-%VERSION%-windows-amd64.zip"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
copy /y "%PROBE%" "%STAGE%\ttt2_setup_probe.exe" >nul || exit /b 1
copy /y "%MANIFEST%" "%STAGE%\recomplauncher.json" >nul || exit /b 1
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%'" || exit /b 1
rmdir /s /q "%STAGE%"
echo Wrote %ZIP%
exit /b 0
