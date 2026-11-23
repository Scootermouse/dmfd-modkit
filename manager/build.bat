@echo off
rem Builds manager\build\DMFD Mod Manager.exe: C# (WPF, made in code) for the .NET Framework that
rem ships with Windows, compiled with the C# compiler from the Visual Studio Build Tools. Nothing is
rem downloaded. Usage: manager\build.bat [install]   (install copies it into the game folder)
setlocal
set HERE=%~dp0
rem The game folder for "install": set DMFD_GAME to it, or Steam's default folder is used.
if defined DMFD_GAME (set "GAME=%DMFD_GAME%") else set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Dragon Marked For Death"
set CSC=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe
set FW=C:\Windows\Microsoft.NET\Framework64\v4.0.30319
if not exist "%HERE%build" mkdir "%HERE%build"
"%CSC%" /nologo /target:winexe /optimize+ /langversion:7.3 /out:"%HERE%build\DMFD Mod Manager.exe" /nostdlib+ /noconfig ^
  /r:"%FW%\mscorlib.dll" /r:"%FW%\System.dll" /r:"%FW%\System.Core.dll" /r:"%FW%\System.Xml.dll" /r:"%FW%\System.Xaml.dll" ^
  /r:"%FW%\WPF\PresentationFramework.dll" /r:"%FW%\WPF\PresentationCore.dll" /r:"%FW%\WPF\WindowsBase.dll" ^
  /r:"%FW%\System.IO.Compression.dll" /r:"%FW%\System.IO.Compression.FileSystem.dll" /r:"%FW%\Microsoft.VisualBasic.dll" ^
  /r:"%FW%\System.Windows.Forms.dll" /r:"%FW%\System.Drawing.dll" /r:"%FW%\System.Web.Extensions.dll" ^
  /win32icon:"%HERE%art\icon.ico" /resource:"%HERE%art\icon.ico",icon.ico ^
  "%HERE%src\*.cs" || exit /b 1
echo Built manager\build\DMFD Mod Manager.exe
if /i "%1"=="install" copy /y "%HERE%build\DMFD Mod Manager.exe" "%GAME%\DMFD Mod Manager.exe" >nul && echo Installed to the game folder.
