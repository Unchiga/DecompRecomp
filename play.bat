@echo off
rem Windows counterpart of play.sh: build anything that changed, then launch the
rem native PC game. "trace" logs unported calls and keeps going; "load [slot]"
rem starts from a save state slot (default 1). Keys and save states are as in
rem build-pc.sh. The game reads your disc image from game\ (any .bin of the USA
rem disc). Needs Python 3 and llvm-mingw on PATH (notes/pc-build.md,
rem "Windows"); the matching build's ELFs come from Linux or WSL.
setlocal
cd /d "%~dp0"
if not exist tmp\project-build\SLUS_014.11.elf goto :no_elf
if not exist tmp\overlays\main_menu\build\main_menu.elf goto :no_elf
if not exist tmp\pc\win32-deps\lib\libfreetype.a (
    python tools\pc\build_win32_deps.py || goto :failed
)
python tools\pc\build_game32.py || goto :failed
if /i "%~1"=="trace" set MEMORIES_STUB_TRACE=1
if /i "%~1"=="load" (
    if "%~2"=="" (set MEMORIES_LOAD_STATE=1) else (set MEMORIES_LOAD_STATE=%~2)
)
for /f %%t in ('python -c "import time; print(int(time.time()))"') do set STARTED=%%t
tmp\pc\game32\memories-pc.exe
set CODE=%errorlevel%
rem A crash code (negative) means Windows ended the game before its own
rem handler could write tmp\pc\crash-*.txt; the report comes from Windows' dump.
if %CODE% LSS 0 (
    echo The game crashed ^(exit code %CODE%^). Writing a report from Windows' crash dump...
    python tools\pc\crash_report.py --since %STARTED% --wait 15
    pause
)
exit /b %CODE%

:no_elf
echo The matching build's ELFs are missing. Run "make match match-overlays" on
echo Linux or WSL and copy tmp\project-build\SLUS_014.11.elf and
echo tmp\overlays\*\build\*.elf into this checkout (notes/pc-build.md, "Windows").
pause
exit /b 1

:failed
echo Build failed; see the messages above.
pause
exit /b 1
