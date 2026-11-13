@echo off
rem builds DeadlockOutlines.exe with MSVC (needs Visual Studio or the Build Tools with the C++ workload)
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if not errorlevel 1 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul

:build
if not exist build mkdir build
cl /nologo /O1 /MT /EHsc /std:c++17 /W3 src\main.cpp /Fo:build\ /Fe:build\DeadlockOutlines.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED
exit /b %errorlevel%

:novs
echo Visual Studio with the C++ workload was not found.
exit /b 1
