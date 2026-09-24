@echo off
rem Play the native PC game: put your disc image (the .bin of the USA disc) in
rem game\, then run this. The first run builds the game, which fetches what it
rem needs into tmp\ (Python if it is not installed, the compiler, CMake and
rem Ninja, and the libraries; a few minutes); later runs rebuild only what
rem changed. "trace" logs unported calls and keeps going; "load [slot]" starts
rem from a save state slot (default 1). play.sh is the Linux counterpart.
setlocal
cd /d "%~dp0"
if not exist game\*.bin (
    if not exist game mkdir game
    echo Put your disc image in the game folder first: the .bin of
    echo Yu-Gi-Oh! Forbidden Memories ^(USA, SLUS-01411^). Any file name ending in .bin.
    pause
    exit /b 1
)

rem Python: the installed one when there is one (not the Microsoft Store
rem placeholder, which fails this check), else a private copy in tmp\pc\tools.
set PYTHON=
python -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>&1 && set PYTHON=python
if not defined PYTHON if exist tmp\pc\tools\python\python.exe set PYTHON=%CD%\tmp\pc\tools\python\python.exe
if not defined PYTHON call :fetch_python || goto :failed

"%PYTHON%" tools\pc\build_game32.py || goto :failed
if /i "%~1"=="trace" set MEMORIES_STUB_TRACE=1
if /i "%~1"=="load" (
    if "%~2"=="" (set MEMORIES_LOAD_STATE=1) else (set MEMORIES_LOAD_STATE=%~2)
)
tmp\pc\game32\memories-pc.exe
set CODE=%errorlevel%
rem The game's monitor (src\pc\debug\monitor.c) has written the report and
rem said where; keep the window open to read it.
if %CODE% NEQ 0 (
    echo.
    echo The game ended with an error ^(exit code %CODE%^). The report is in %CD%\tmp\pc:
    echo the newest crash-*.txt or hang-*.txt, with last-session.log. Please send them.
    pause
)
exit /b %CODE%

:fetch_python
rem The official embeddable Python, checked against its SHA-256.
echo Fetching Python 3.14.7 into tmp\pc\tools\python (once)...
if not exist tmp\pc\tools\downloads mkdir tmp\pc\tools\downloads
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference = 'Stop'; [Net.ServicePointManager]::SecurityProtocol = 'Tls12';" ^
    "$zip = 'tmp\pc\tools\downloads\python-3.14.7-embed-amd64.zip';" ^
    "if (-not (Test-Path $zip)) { Invoke-WebRequest -UseBasicParsing -OutFile $zip -Uri 'https://www.python.org/ftp/python/3.14.7/python-3.14.7-embed-amd64.zip' };" ^
    "if ((Get-FileHash -Algorithm SHA256 $zip).Hash -ne 'd297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15') { Remove-Item $zip; throw 'the Python download does not match its SHA-256; run play.bat again' };" ^
    "Expand-Archive -Force $zip 'tmp\pc\tools\python'"
if errorlevel 1 exit /b 1
set PYTHON=%CD%\tmp\pc\tools\python\python.exe
exit /b 0

:failed
echo Build failed; see the messages above.
pause
exit /b 1
