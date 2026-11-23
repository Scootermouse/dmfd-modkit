@echo off
rem Builds DataMod from the spreadsheets in the mod folder and installs it.
call "%~dp0tools\py.bat" "%~dp0tools\build_mod.py"
echo.
pause
