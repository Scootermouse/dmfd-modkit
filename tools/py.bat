@echo off
rem Runs a tool with the Python that comes with the kit (tools\python), or the installed one.
rem usage: call "%%~dp0py.bat" <script.py> [arguments]
if exist "%~dp0python\python.exe" (
  "%~dp0python\python.exe" -I %*
) else (
  where python >nul 2>nul || (echo Python not found: tools\python is missing and Python is not installed. & exit /b 1)
  python -I %*
)
