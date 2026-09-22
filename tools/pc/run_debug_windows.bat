@echo off
rem Play the Windows build with problem reporting on (notes/pc-build.md, "Windows"):
rem  - a state every 30 s in tmp\pc\debug\states (auto1..auto3, the newest overwrites the oldest);
rem    your own F5 states stay in Documents\My Games\YFM Re-Decomp\states,
rem  - traces in tmp\pc\debug\trace.log and everything the console shows in tmp\pc\debug\console.txt,
rem  - tmp\pc\hang-*.txt when the game stops calling VSync for 5 s (window frozen),
rem  - tmp\pc\crash-*.txt when it crashes.
rem Nothing needs describing afterwards: those files say where it stopped.
cd /d "%~dp0..\.."
if not exist tmp\pc\debug\previous mkdir tmp\pc\debug\previous
rem Reports from earlier runs move aside, so what is left in tmp\pc is from this one.
move /y tmp\pc\hang-*.txt tmp\pc\debug\previous\ >nul 2>&1
move /y tmp\pc\crash-*.txt tmp\pc\debug\previous\ >nul 2>&1
set MEMORIES_AUTOSAVE=30
set MEMORIES_AUTOSAVE_DIR=tmp/pc/debug/states
set MEMORIES_LOG=tmp/pc/debug/trace.log
set MEMORIES_TRACE=frames,state,memcard,model,duel_effects,mips_printf,window,audio,clock
echo ==== %date% %time% ==== >> tmp\pc\debug\trace.log
echo Playing with problem reporting on. Reports go to %cd%\tmp\pc\debug and tmp\pc.
for /f %%t in ('python -c "import time; print(int(time.time()))"') do set STARTED=%%t
tmp\pc\game32\memories-pc.exe game\SLUS_014.11 > tmp\pc\debug\console.txt 2>&1
set CODE=%errorlevel%
echo Game exited with code %CODE%.
rem Crashes Windows ends before the game's handler runs: report from its dump.
if %CODE% LSS 0 python tools\pc\crash_report.py --since %STARTED% --wait 15
dir /b /o-d tmp\pc\hang-*.txt tmp\pc\crash-*.txt 2>nul
pause
