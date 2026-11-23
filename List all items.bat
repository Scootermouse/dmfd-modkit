@echo off
rem Writes mod\all_items.csv: every item in the game, for reference.
call "%~dp0tools\py.bat" "%~dp0tools\build_mod.py" --list
echo.
pause
