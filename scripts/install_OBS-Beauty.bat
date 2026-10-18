@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
title OBS-Beauty Setup

set "beauty_source=%~dp0obs-beauty"
if not defined ProgramData (
    set "beauty_error=Cannot locate the system ProgramData directory."
    goto failed
)
set "beauty_target=%ProgramData%\obs-studio\plugins\obs-beauty"

if not exist "%beauty_source%\bin\64bit\obs-beauty.dll" goto missing_payload
if not exist "%beauty_source%\data\beauty.effect" goto missing_payload
if not exist "%beauty_source%\data\locale\en-US.ini" goto missing_payload
if not exist "%beauty_source%\data\locale\zh-CN.ini" goto missing_payload
if not exist "%beauty_source%\data\locale\zh-TW.ini" goto missing_payload

tasklist /fi "IMAGENAME eq obs64.exe" /nh 2>nul | find /i "obs64.exe" >nul
if not errorlevel 1 (
    set "beauty_error=Close OBS Studio, then run install_OBS-Beauty.bat again."
    goto failed
)

echo Installing OBS-Beauty...
echo Destination: "%beauty_target%"
robocopy "%beauty_source%" "%beauty_target%" /e /copy:DAT /dcopy:DAT /r:1 /w:1 /njh /njs /nfl /ndl /np >nul 2>&1
if errorlevel 8 (
    set "beauty_error=File copy failed. If access is denied, right-click install_OBS-Beauty.bat and select Run as administrator."
    goto failed
)

echo.
echo Installation completed. Restart OBS and add this plugin from a source's Effect Filters.
powershell.exe -NoLogo -NoProfile -Command "if([Console]::IsInputRedirected){exit}; for($i=5; $i -gt 0; $i--){[Console]::Write([char]13+'Closing in '+$i+' seconds. Press any key to exit... '); $until=[DateTime]::UtcNow.AddSeconds(1); do{if([Console]::KeyAvailable){$null=[Console]::ReadKey($true); [Console]::WriteLine(); exit}; Start-Sleep -Milliseconds 50}while([DateTime]::UtcNow -lt $until)}; [Console]::WriteLine()"
exit /b 0

:missing_payload
set "beauty_error=Missing plugin files. Extract the entire ZIP and keep install_OBS-Beauty.bat next to the obs-beauty folder."

:failed
echo.
echo Installation failed: %beauty_error%
echo Press any key to exit.
pause >nul
exit /b 1
