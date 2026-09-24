#!/usr/bin/env python3
"""Check that every kind of failure ends in a report.

Runs the game headless with MEMORIES_CRASH_TEST=<kind>@<frame>
(src/pc/debug/crash_test.c) in a scratch folder, one kind at a time, and
checks what the monitor (src/pc/debug/monitor.c) and the game's own handler
(src/pc/debug/crash.c) wrote. A freeze is ended once its report is there, as
a player would close the window.

    python3 tools/pc/crash_check.py              every kind, Linux build
    python3 tools/pc/crash_check.py --windows    the Windows build (under Wine off Windows)
    python3 tools/pc/crash_check.py segv hang    only these

Reports are left in tmp/pc/crash-check/<kind>/user/reports to read.
"""
import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/pc"))
import smoke  # noqa: E402  (launcher: how Wine runs the Windows build)

OUTPUT = ROOT / "tmp/pc/crash-check"
FRAME = 200

# kind: (report, ends by itself, what the report says on Linux, on Windows)
COMMON_CRASH = ["build: ", "os: ", "cpu: ", "settings: ", "the game's log", "the game's console output"]
CASES = {
    "segv": ("crash", True, ["the game crashed: signal 11", "fatal signal 11", "CrashTest_Frame", "core dump"],
             ["the game crashed: exception 0xc0000005", "fatal exception 0xc0000005", "CrashTest_Frame"]),
    "null": ("crash", True, ["the game crashed: signal 11", "fatal signal 11"],
             ["the game crashed: exception 0xc0000005", "fatal exception 0xc0000005"]),
    "thread": ("crash", True, ["the game crashed: signal 11", "crash_thread"],
               ["the game crashed: exception 0xc0000005", "crash_thread"]),
    "overflow": ("crash", True, ["the game crashed: signal 11", "fatal signal 11", "recurse"],
                 ["the game crashed: exception 0xc00000fd", "fatal exception 0xc00000fd"]),
    "abort": ("crash", True, ["the game crashed: signal 6", "fatal signal 6"],
              ["abort()", "fatal: abort()"]),
    "fatal": ("crash", True, ["exit code 70", "fatal: crash test"], ["exit code 70", "fatal: crash test"]),
    "kill": ("crash", True, ["the game was killed (SIGKILL)"],
             ["exception 0xc0000409", "the game's own crash handler did not run"]),
    "hang": ("hang", False, ["no frame for", "no VSync for 5 s", "CrashTest_Frame", "closed while not responding"],
             ["no frame for", "no VSync for 5 s", "CrashTest_Frame", "closed while not responding"]),
    "spin": ("hang", False, ["no frame for", "CrashTest_Frame", "closed while not responding"],
             ["no frame for", "CrashTest_Frame", "closed while not responding"]),
    "deadlock": ("hang", False, ["no frame for", "futex", "CrashTest_Frame", "closed while not responding"],
                 ["no frame for", "CrashTest_Frame", "closed while not responding"]),
    "tickhang": ("hang", False, ["no frame for", "on_alarm", "closed while not responding"],
                 ["no frame for", "closed while not responding"]),
    "slow": ("hang", True, ["no frame for", "went on after"], ["no frame for", "went on after"]),
    "restart": ("crash", True, ["exit code 70", "after the restart"], ["exit code 70", "after the restart"]),
}


def environment(user: Path, kind: str, windows: bool) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items() if not key.startswith("MEMORIES_")}
    images = sorted((ROOT / "game").glob("*.bin"))
    if not images:
        sys.exit("crash_check: no disc image in game/")
    env.update({
        "MEMORIES_DISC": str(images[0]),
        "MEMORIES_USER_DIR": str(user),
        "MEMORIES_HEADLESS": "1",
        "MEMORIES_NO_AUDIO": "1",
        "MEMORIES_NO_GAMEPAD": "1",
        "MEMORIES_SPEED": "-1",
        "MEMORIES_CRASH_TEST": f"{kind}@{FRAME}",
    })
    if kind == "slow":
        # Ends by itself: a frame dump well after the long wait.
        env["MEMORIES_DUMP_FRAME"] = str(FRAME + 100)
        env["MEMORIES_DUMP_PATH"] = str(user / "frame.ppm")
    return env


def wait_for(path_glob, folder: Path, text: str, seconds: float):
    deadline = time.time() + seconds
    while time.time() < deadline:
        for path in folder.glob(path_glob):
            if text in path.read_text(errors="replace"):
                return path
        time.sleep(0.25)
    return None


def end_frozen(pid: int, windows: bool, extra: dict[str, str]) -> None:
    """Close the frozen game as a player would: the task manager's End task."""
    if windows and sys.platform != "win32":
        subprocess.run(["wine", "taskkill", "/f", "/pid", str(pid)], env={**os.environ, **extra},
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    elif windows:
        subprocess.run(["taskkill", "/f", "/pid", str(pid)], stdout=subprocess.DEVNULL, check=False)
    else:
        os.kill(pid, signal.SIGTERM)


def check(kind: str, executable: Path, windows: bool) -> bool:
    report_kind, ends, linux_words, windows_words = CASES[kind]
    words = (windows_words if windows else linux_words) + (COMMON_CRASH if report_kind == "crash" else [])
    folder = OUTPUT / kind
    shutil.rmtree(folder, ignore_errors=True)
    user = folder / "user"
    user.mkdir(parents=True)
    reports = user / "reports"
    command, extra = smoke.launcher(executable)
    started = time.time()
    with open(folder / "console.txt", "w") as console:
        # The scratch folder has no tmp/pc, so the reports go to the user folder.
        game = subprocess.Popen(command, cwd=folder, env={**environment(user, kind, windows), **extra},
                                stdout=console, stderr=subprocess.STDOUT)
        try:
            if not ends:
                report = wait_for("hang-*.txt", reports, "==== monitor: no ", 90)
                if not report:
                    print(f"crash_check: {kind}: no hang report after 90 s", file=sys.stderr)
                    game.kill()
                    return False
                time.sleep(1)
                end_frozen(int(re.findall(r"\d+", report.name)[-1]), windows, extra)
            game.wait(timeout=120)
        except subprocess.TimeoutExpired:
            game.kill()
            print(f"crash_check: {kind}: the game did not end", file=sys.stderr)
            return False
    found = sorted(reports.glob(f"{report_kind}-*.txt")) if reports.is_dir() else []
    if not found:
        print(f"crash_check: {kind}: no {report_kind}-*.txt in {reports} (exit {game.returncode})", file=sys.stderr)
        return False
    text = "".join(path.read_text(errors="replace") for path in found)
    missing = [word for word in words if word not in text]
    session = reports / "last-session.log"
    if not session.is_file() or f"crash test: {kind} now" not in session.read_text(errors="replace"):
        missing.append("last-session.log with the game's output")
    home = os.environ.get("HOME", "")
    if not windows and home and home in text.split("==== monitor:", 1)[-1]:
        missing.append("no home folder in the monitor's section")
    if kind == "slow" and game.returncode != 0:
        missing.append("exit status 0 after the long wait")
    if missing:
        print(f"crash_check: {kind}: FAILED (exit {game.returncode}); the report lacks: {missing}\n"
              f"  {', '.join(str(path) for path in found)}", file=sys.stderr)
        return False
    print(f"crash_check: {kind} passed ({time.time() - started:.1f} s, exit {game.returncode}): "
          f"{', '.join(path.name for path in found)}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("kinds", nargs="*", help=f"some of: {' '.join(CASES)}")
    parser.add_argument("--windows", action="store_true", help="the Windows build (under Wine off Windows)")
    arguments = parser.parse_args()
    unknown = [kind for kind in arguments.kinds if kind not in CASES]
    if unknown:
        parser.error(f"no such kind: {' '.join(unknown)}")
    executable = smoke.WINDOWS_EXECUTABLE if arguments.windows else smoke.DEFAULT_EXECUTABLE
    if not executable.is_file():
        sys.exit(f"crash_check: {executable} is not built")
    results = [check(kind, executable, arguments.windows) for kind in (arguments.kinds or CASES)]
    print(f"crash_check: {sum(results)} of {len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
