@echo off
rem Developers only: compiles build\winmm.dll (32-bit): the mod loader, the content engine and
rem the in-game menu. Usage: tools\build_dll.bat [install]
rem The code mods (Rebalance, Quick Spells, ...) build with tools\build_codemods.bat.
rem To build your content from the mod folder, double-click "Build mod.bat" instead.
setlocal
rem ROOT as a full path, so /d1trimfile can keep the build folder out of the DLL (assert paths)
for %%I in ("%~dp0..") do set "ROOT=%%~fI\"
rem The game folder for "install": set DMFD_GAME to it, or Steam's default folder is used.
if defined DMFD_GAME (set "GAME=%DMFD_GAME%") else set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Dragon Marked For Death"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat" 10.0.19041.0 >nul || exit /b 1
if not exist "%ROOT%build" mkdir "%ROOT%build"
cd /d "%ROOT%build"
set MH=%ROOT%third_party\minhook\src
set IG=%ROOT%third_party\imgui
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /LD /MP "/d1trimfile:%ROOT%\" ^
   /I"%ROOT%third_party\minhook\include" /I"%IG%" /I"%IG%\backends" ^
   "%ROOT%src\proxy.cpp" "%ROOT%src\datamod.cpp" "%ROOT%src\items.cpp" "%ROOT%src\icons.cpp" "%ROOT%src\drops.cpp" "%ROOT%src\classes.cpp" "%ROOT%src\classfiles.cpp" "%ROOT%src\net.cpp" "%ROOT%src\loader.cpp" "%ROOT%src\menu.cpp" "%ROOT%src\startup.cpp" ^
   "%IG%\imgui.cpp" "%IG%\imgui_draw.cpp" "%IG%\imgui_tables.cpp" "%IG%\imgui_widgets.cpp" ^
   "%IG%\backends\imgui_impl_dx9.cpp" "%IG%\backends\imgui_impl_win32.cpp" ^
   "%MH%\buffer.c" "%MH%\hook.c" "%MH%\trampoline.c" "%MH%\hde\hde32.c" ^
   /Fe:winmm.dll /link /DEF:"%ROOT%src\winmm.def" /MAP user32.lib d3d9.lib || exit /b 1
if /i "%1"=="install" copy /y winmm.dll "%GAME%\winmm.dll" >nul && echo Installed to game folder.
