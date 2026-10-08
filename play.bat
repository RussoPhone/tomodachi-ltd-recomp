@echo off
rem Starts the game. Options: --fullscreen   --scale 2 (sharper image; 1, 1.5, 2, 3 or 4)
if not exist "%~dp0local\package\tomodachi\run.bat" (
  echo The game is not installed yet. Run install.bat first.
  pause
  exit /b 1
)
call "%~dp0local\package\tomodachi\run.bat" %*
