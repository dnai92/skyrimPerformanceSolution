@echo off
rem Baut SPS.dll (RelWithDebInfo) -> build\release\SPS.dll
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cmake --preset release || exit /b 1
cmake --build --preset release || exit /b 1
echo.
echo Fertig: %~dp0build\release\SPS.dll
