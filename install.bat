@echo off
rem Installs the native build of Tomodachi Life: Living the Dream from YOUR copy of the game.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows\install.ps1" %*
echo.
pause
