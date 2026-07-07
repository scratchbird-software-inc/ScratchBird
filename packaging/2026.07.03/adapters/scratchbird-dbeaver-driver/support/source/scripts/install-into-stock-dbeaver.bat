REM Copyright (c) 2026 ScratchBird Software Inc.
REM
REM This Source Code Form is subject to the terms of the Mozilla Public
REM License, v. 2.0. If a copy of the MPL was not distributed with this
REM file, You can obtain one at https://mozilla.org/MPL/2.0/.
REM
REM SPDX-License-Identifier: MPL-2.0

@echo off
setlocal

if /I "%~1"=="-h" goto :usage
if /I "%~1"=="--help" goto :usage
if not "%~3"=="" goto :usage

set "INSTALL_HINT="
set "INSTALL_DIR="
set "ZIP="
set "SCRIPT_DIR=%~dp0"
set "DBEAVER_EXE="

if not "%~1"=="" (
  if /I "%~x1"==".zip" (
    set "ZIP=%~f1"
  ) else (
    set "INSTALL_HINT=%~1"
  )
)

if not "%~2"=="" set "ZIP=%~f2"

if defined INSTALL_HINT call :resolve_install_hint "%INSTALL_HINT%"
if not defined INSTALL_DIR call :auto_detect_install

if not defined INSTALL_DIR (
  echo Could not determine a DBeaver install location. Pass the install root or launcher path explicitly.
  exit /b 1
)

if not defined ZIP call :find_zip

if not defined ZIP (
  echo ScratchBird update-site zip not found. Pass it explicitly or place it next to this script.
  exit /b 1
)

if not exist "%ZIP%" (
  echo ScratchBird update-site zip does not exist: "%ZIP%"
  exit /b 1
)

set "ZIP_URI=%ZIP:\=/%"
set "ROOTS_FILE=%TEMP%\scratchbird_dbeaver_roots_%RANDOM%%RANDOM%.txt"

echo Installing ScratchBird into "%INSTALL_DIR%"
echo Using update-site archive "%ZIP%"

rem Replace any existing ScratchBird feature first so p2 doesn't fail on version conflicts.
"%DBEAVER_EXE%" -nosplash -consoleLog ^
  -application org.eclipse.equinox.p2.director ^
  -uninstallIU org.jkiss.dbeaver.ext.scratchbird.feature.feature.group ^
  -destination "%INSTALL_DIR%" ^
  -profile DefaultProfile ^
  -bundlepool "%INSTALL_DIR%" ^
  -profileProperties org.eclipse.update.install.features=true ^
  -purgeHistory

"%DBEAVER_EXE%" -nosplash -consoleLog ^
  -application org.eclipse.equinox.p2.director ^
  -repository "jar:file:/%ZIP_URI%!/" ^
  -installIU org.jkiss.dbeaver.ext.scratchbird.feature.feature.group ^
  -destination "%INSTALL_DIR%" ^
  -profile DefaultProfile ^
  -bundlepool "%INSTALL_DIR%" ^
  -profileProperties org.eclipse.update.install.features=true

if errorlevel 1 exit /b %errorlevel%

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
findstr /C:"org.jkiss.dbeaver.ext.scratchbird.feature.feature.group" "%ROOTS_FILE%" >nul
if errorlevel 1 (
  echo ScratchBird feature was not found in the installed roots output.
  del "%ROOTS_FILE%" >nul 2>&1
  exit /b 1
)

del "%ROOTS_FILE%" >nul 2>&1

echo ScratchBird install completed successfully.
echo Installed root confirmed: org.jkiss.dbeaver.ext.scratchbird.feature.feature.group
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

:find_zip
for /f "delims=" %%I in ('dir /b /a:-d /o:-d "%SCRIPT_DIR%scratchbird-dbeaver-update-site-*.zip" 2^>nul') do (
  set "ZIP=%SCRIPT_DIR%%%I"
  goto :eof
)
goto :eof

:usage
echo Usage:
echo   install-into-stock-dbeaver.bat [path-to-dbeaver-install-or-launcher] [path-to-scratchbird-dbeaver-update-site-*.zip]
echo.
echo Description:
echo   Installs the ScratchBird DBeaver feature into an existing packaged
echo   DBeaver application using the p2 director. If no install path is
echo   provided, the script attempts to discover DBeaver from DBEAVER_HOME,
echo   PATH, and common install locations. If no zip path is provided, the
echo   script looks for a bundled scratchbird-dbeaver-update-site-*.zip next
echo   to itself.
exit /b 1
