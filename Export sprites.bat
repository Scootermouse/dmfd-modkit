@echo off
rem Writes every animation frame of the six class bodies to mod\sprites_original (takes a minute or two).
call "%~dp0tools\py.bat" "%~dp0tools\build_mod.py" --export-sprites
echo.
pause
