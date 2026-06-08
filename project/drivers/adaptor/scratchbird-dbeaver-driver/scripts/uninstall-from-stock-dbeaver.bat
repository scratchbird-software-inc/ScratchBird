REM Copyright (c) 2026 ScratchBird Software Inc.
REM
REM This Source Code Form is subject to the terms of the Mozilla Public
REM License, v. 2.0. If a copy of the MPL was not distributed with this
REM file, You can obtain one at https://mozilla.org/MPL/2.0/.
REM
REM SPDX-License-Identifier: MPL-2.0

@echo off
setlocal

set "FEATURE_IU=org.jkiss.dbeaver.ext.scratchbird.feature.feature.group"

if /I "%~1"=="-h" goto :usage
if /I "%~1"=="--help" goto :usage
if not "%~2"=="" goto :usage

set "INSTALL_HINT=%~1"
set "INSTALL_DIR="
set "SCRIPT_DIR=%~dp0"
set "DBEAVER_EXE="
set "ROOTS_FILE=%TEMP%\scratchbird_dbeaver_roots_%RANDOM%%RANDOM%.txt"

if defined INSTALL_HINT call :resolve_install_hint "%INSTALL_HINT%"
if not defined INSTALL_DIR call :auto_detect_install

if not defined INSTALL_DIR (
  echo Could not determine a DBeaver install location. Pass the install root or launcher path explicitly.
  exit /b 1
)

echo Checking ScratchBird installation in "%INSTALL_DIR%"

"%DBEAVER_EXE%" -nosplash -consoleLog ^
  -application org.eclipse.equinox.p2.director ^
  -destination "%INSTALL_DIR%" ^
  -profile DefaultProfile ^
  -bundlepool "%INSTALL_DIR%" ^
  -listInstalledRoots > "%ROOTS_FILE%" 2>&1

if errorlevel 1 (
  type "%ROOTS_FILE%"
  del "%ROOTS_FILE%" >nul 2>&1
  exit /b %errorlevel%
)

type "%ROOTS_FILE%"
findstr /C:"%FEATURE_IU%" "%ROOTS_FILE%" >nul
if errorlevel 1 (
  echo ScratchBird is not currently installed in this DBeaver instance.
  echo Launcher: "%DBEAVER_EXE%"
  del "%ROOTS_FILE%" >nul 2>&1
  exit /b 0
)

echo Removing ScratchBird from "%INSTALL_DIR%"

"%DBEAVER_EXE%" -nosplash -consoleLog ^
  -application org.eclipse.equinox.p2.director ^
  -uninstallIU "%FEATURE_IU%" ^
  -destination "%INSTALL_DIR%" ^
  -profile DefaultProfile ^
  -bundlepool "%INSTALL_DIR%" ^
  -profileProperties org.eclipse.update.install.features=true ^
  -purgeHistory

if errorlevel 1 (
  del "%ROOTS_FILE%" >nul 2>&1
  exit /b %errorlevel%
)

"%DBEAVER_EXE%" -nosplash -consoleLog ^
  -application org.eclipse.equinox.p2.director ^
  -destination "%INSTALL_DIR%" ^
  -profile DefaultProfile ^
  -bundlepool "%INSTALL_DIR%" ^
  -listInstalledRoots > "%ROOTS_FILE%" 2>&1

if errorlevel 1 (
  type "%ROOTS_FILE%"
  del "%ROOTS_FILE%" >nul 2>&1
  exit /b %errorlevel%
)

type "%ROOTS_FILE%"
findstr /C:"%FEATURE_IU%" "%ROOTS_FILE%" >nul
if not errorlevel 1 (
  echo ScratchBird feature is still present in the installed roots output.
  del "%ROOTS_FILE%" >nul 2>&1
  exit /b 1
)

del "%ROOTS_FILE%" >nul 2>&1

echo ScratchBird uninstall completed successfully.
echo Removed root: %FEATURE_IU%
exit /b 0

:resolve_install_hint
if exist "%~1\dbeaver.exe" (
  set "INSTALL_DIR=%~f1"
  set "DBEAVER_EXE=%~f1\dbeaver.exe"
  goto :eof
)
if exist "%~1\DBeaver.exe" (
  set "INSTALL_DIR=%~f1"
  set "DBEAVER_EXE=%~f1\DBeaver.exe"
  goto :eof
)
if exist "%~f1" (
  if /I "%~x1"==".exe" (
    if /I "%~n1"=="dbeaver" (
      set "INSTALL_DIR=%~dp1"
      if "%INSTALL_DIR:~-1%"=="\" set "INSTALL_DIR=%INSTALL_DIR:~0,-1%"
      set "DBEAVER_EXE=%~f1"
      goto :eof
    )
  )
)
goto :eof

:auto_detect_install
if defined DBEAVER_HOME (
  call :resolve_install_hint "%DBEAVER_HOME%"
  if defined INSTALL_DIR goto :eof
)

for /f "delims=" %%I in ('where dbeaver.exe 2^>nul') do (
  if not defined INSTALL_DIR call :resolve_install_hint "%%~fI"
)
if defined INSTALL_DIR goto :eof

for %%I in (
  "%ProgramFiles%\DBeaver"
  "%ProgramFiles%\DBeaverCE"
  "%ProgramFiles%\DBeaver Community"
  "%ProgramFiles(x86)%\DBeaver"
  "%ProgramFiles(x86)%\DBeaverCE"
  "%LocalAppData%\DBeaver"
) do (
  if not defined INSTALL_DIR call :resolve_install_hint "%%~I"
)
goto :eof

:usage
echo Usage:
echo   uninstall-from-stock-dbeaver.bat [path-to-dbeaver-install-or-launcher]
echo.
echo Description:
echo   Removes the ScratchBird DBeaver feature from an existing packaged
echo   DBeaver application using the p2 director. If no install path is
echo   provided, the script attempts to discover DBeaver from DBEAVER_HOME,
echo   PATH, and common install locations.
exit /b 1
