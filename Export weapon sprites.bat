@echo off
rem Writes every animation frame of every weapon sprite to mod\sprites_original (takes about two minutes),
rem plus mod\sprites_original\weapons.csv: which items use which weapon sprite.
call "%~dp0tools\py.bat" "%~dp0tools\build_mod.py" --export-weapons
echo.
pause
