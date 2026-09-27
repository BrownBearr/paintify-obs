@echo off
REM Double-click to open gpu-sbr. You can also drag an image file onto this
REM .bat to open that image directly.
if "%~1"=="" (
  start "" "%~dp0build\gpu-sbr.exe"
) else (
  start "" "%~dp0build\gpu-sbr.exe" --in "%~1"
)
