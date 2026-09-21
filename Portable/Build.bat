@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title VisualMediaPlayer - Portable Build

echo.
echo Building VisualMediaPlayer Portable...
echo Synchronizing release version...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\tools\SyncVersion.ps1" -Root "%~dp0.."
if errorlevel 1 (
  echo ERROR: Could not synchronize Version.props.
  if not defined CI pause
  exit /b 1
)

echo.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: Visual Studio C++ compiler was not found.
  echo Install the Desktop development with C++ workload, then run this again.
  if not defined CI pause
  exit /b 1
)

set "VS="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo ERROR: Visual Studio C++ x64 tools were not found.
  if not defined CI pause
  exit /b 1
)

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo ERROR: Could not initialize the Visual Studio x64 compiler.
  if not defined CI pause
  exit /b 1
)

set "APP_PROJECT=%~dp0Source\VisualMediaPlayer.vcxproj"
set "APP_EXE=%~dp0Source\x64\Release\VisualMediaPlayer.exe"
set "FINAL_EXE=%~dp0VisualMediaPlayer.exe"

if exist "%FINAL_EXE%" del /q "%FINAL_EXE%"

echo Building VisualMediaPlayer.exe...
msbuild "%APP_PROJECT%" /t:Clean /m /nologo /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 >nul
msbuild "%APP_PROJECT%" /t:Build /m /nologo /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145
if errorlevel 1 goto :failed

if not exist "%APP_EXE%" (
  echo ERROR: VisualMediaPlayer.exe was not produced.
  goto :failed
)

copy /y "%APP_EXE%" "%FINAL_EXE%" >nul
if errorlevel 1 goto :failed

call :sign_optional "%FINAL_EXE%"
if errorlevel 1 goto :failed

call :cleanup

echo.
echo ========================================
echo DONE
echo ========================================
echo.
echo VisualMediaPlayer.exe is ready.
echo ButtonAssets\ contains the replaceable button artwork.
echo Replace a PNG or SVG there and restart the app -- no rebuild needed.
echo.
if not defined CI pause
exit /b 0

:failed
echo.
echo BUILD FAILED. Send me a screenshot of the red/error text above.
echo.
call :cleanup
if not defined CI pause
exit /b 1

:sign_optional
if not defined VMP_SIGN_CERT_SHA1 if not defined VMP_SIGN_PFX (
  echo Code signing not configured; leaving build unsigned.
  exit /b 0
)
echo Signing %~nx1...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\tools\SignRelease.ps1" "%~1"
exit /b %errorlevel%

:cleanup
if exist "%~dp0Source\x64" rmdir /s /q "%~dp0Source\x64" 2>nul
for /d %%D in ("%~dp0Source\VisualMe.*") do rmdir /s /q "%%~fD" 2>nul
exit /b 0
