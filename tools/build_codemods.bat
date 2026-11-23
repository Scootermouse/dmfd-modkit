@echo off
rem Developers only: compiles the code mods in codemods\ (32-bit) into build\mods\<Name>\.
rem Usage: tools\build_codemods.bat [install]
rem   install copies each mod's DLL and mod.ini to <game>\mods\<Name>\ (settings files there
rem   are kept; an existing mod.ini's enabled= setting is kept too).
setlocal
rem ROOT as a full path, so /d1trimfile can keep the build folder out of the DLLs (assert paths)
for %%I in ("%~dp0..") do set "ROOT=%%~fI\"
rem The game folder for "install": set DMFD_GAME to it, or Steam's default folder is used.
if defined DMFD_GAME (set "GAME=%DMFD_GAME%") else set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Dragon Marked For Death"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat" 10.0.19041.0 >nul || exit /b 1
set MH=%ROOT%third_party\minhook\src
set IG=%ROOT%third_party\imgui
set CM=%ROOT%codemods
set COMMON="%CM%\common\modbase.cpp" "%IG%\imgui.cpp" "%IG%\imgui_draw.cpp" "%IG%\imgui_tables.cpp" "%IG%\imgui_widgets.cpp" ^
   "%MH%\buffer.c" "%MH%\hook.c" "%MH%\trampoline.c" "%MH%\hde\hde32.c"
if /i "%1"=="install" set INSTALL=1
set CFLAGS=/nologo /O2 /MT /EHsc /std:c++17 /W3 /LD /MP "/d1trimfile:%ROOT%\" /I"%ROOT%third_party\minhook\include" /I"%IG%"

call :build Rebalance "%CM%\Rebalance\main.cpp" "%CM%\Rebalance\patches.cpp" "%CM%\Rebalance\settings.cpp" "%CM%\Rebalance\net.cpp" "%CM%\Rebalance\unlocks.cpp" "%CM%\Rebalance\tabs.cpp" || exit /b 1
call :build QuickSpells "%CM%\QuickSpells\main.cpp" "%CM%\QuickSpells\quickspell.cpp" || exit /b 1
call :build SkipQuit "%CM%\SkipQuit\main.cpp" || exit /b 1
call :build HolyUp "%CM%\HolyUp\main.cpp" || exit /b 1
exit /b 0

rem :build <folder> <sources...>
:build
set FOLDER=%~1
shift
set SRC=
:args
if "%~1"=="" goto compile
set SRC=%SRC% "%~1"
shift
goto args
:compile
if not exist "%ROOT%build\mods\%FOLDER%" mkdir "%ROOT%build\mods\%FOLDER%"
pushd "%ROOT%build\mods\%FOLDER%"
cl %CFLAGS% %SRC% %COMMON% /Fe:%FOLDER%.dll /link /MAP user32.lib xinput.lib || (popd & exit /b 1)
popd
echo Built %FOLDER%.dll
if /i not "%INSTALL%"=="1" exit /b 0
if not exist "%GAME%\mods\%FOLDER%" mkdir "%GAME%\mods\%FOLDER%"
copy /y "%ROOT%build\mods\%FOLDER%\%FOLDER%.dll" "%GAME%\mods\%FOLDER%\" >nul || exit /b 1
set KEEP=1
if exist "%GAME%\mods\%FOLDER%\mod.ini" for /f "tokens=2 delims==" %%e in ('findstr /b /i "enabled=" "%GAME%\mods\%FOLDER%\mod.ini"') do set KEEP=%%e
copy /y "%CM%\%FOLDER%\mod.ini" "%GAME%\mods\%FOLDER%\mod.ini" >nul || exit /b 1
if "%KEEP%"=="0" powershell -NoProfile -Command "(Get-Content '%GAME%\mods\%FOLDER%\mod.ini') -replace '^enabled=1','enabled=0' | Set-Content '%GAME%\mods\%FOLDER%\mod.ini'"
echo Installed to mods\%FOLDER%
exit /b 0
