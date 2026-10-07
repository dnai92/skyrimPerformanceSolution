@echo off
rem Baut SPS.dll (RelWithDebInfo). Aufruf: build.cmd [release|vr] -> build\<preset>\SPS.dll
setlocal
set PRESET=%1
if "%PRESET%"=="" set PRESET=release
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
echo.
echo Fertig: %~dp0build\%PRESET%\SPS.dll
