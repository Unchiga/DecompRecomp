#!/usr/bin/env python3
"""Run deterministic native-game screenshots, the mod export check and the portable PC CTests."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EXECUTABLE = ROOT / "tmp/pc/game32/memories-pc"
WINDOWS_EXECUTABLE = ROOT / "tmp/pc/win32/memories-pc.exe"  # build_game32.py --target windows
WINE_PREFIX = ROOT / "tmp/pc/wine-prefix"
DEFAULT_BUILD = ROOT / "tmp/pc/cmake-test"
FIXTURES = ROOT / "tests/pc/smoke"
OUTPUT = ROOT / "tmp/pc/smoke"


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def smoke_environment(case: dict[str, object], image: Path, settings: Path) -> dict[str, str]:
    preserved_disc = os.environ.get("MEMORIES_DISC")
    environment = {key: value for key, value in os.environ.items() if not key.startswith("MEMORIES_")}
    if preserved_disc:
        environment["MEMORIES_DISC"] = preserved_disc
    environment.update(
        {
            "MEMORIES_HEADLESS": "1",
            "MEMORIES_NO_AUDIO": "1",
            "MEMORIES_NO_GAMEPAD": "1",
            "MEMORIES_SPEED": "-1",
            "MEMORIES_SHOW_HUD": "0",
            "MEMORIES_SETTINGS": str(settings),
            "MEMORIES_INPUT": str(case["input"]),
            "MEMORIES_DUMP_FRAME": str(case["frame"]),
            "MEMORIES_DUMP_PATH": str(image),
            "MEMORIES_WATCHDOG": "0",
        }
    )
    return environment


def launcher(executable: Path) -> tuple[list[str], dict[str, str]]:
    """The command that runs the executable, and what Wine needs when a
    Windows build is tested on another host: a prefix of its own, without the
    Mono and Gecko installers, and quiet."""
    if executable.suffix != ".exe" or sys.platform == "win32":
        return [str(executable)], {}
    return ["wine", str(executable)], {
        "WINEPREFIX": str(WINE_PREFIX),
        "WINEDLLOVERRIDES": "mscoree,mshtml=",
        "WINEDEBUG": "-all",
    }


def run_smoke(executable: Path, record: bool) -> bool:
    if not executable.is_file():
        print(f"smoke: executable is missing: {executable}", file=sys.stderr)
        return False
    OUTPUT.mkdir(parents=True, exist_ok=True)
    command, extra = launcher(executable)
    fixtures = sorted(FIXTURES.glob("*.json"))
    if not fixtures:
        print(f"smoke: no fixtures in {FIXTURES}", file=sys.stderr)
        return False
    for fixture in fixtures:
        case = json.loads(fixture.read_text(encoding="utf-8"))
        name = str(case["name"])
        image = OUTPUT / f"{name}.ppm"
        settings = OUTPUT / f"{name}.settings"
        settings.write_text("", encoding="utf-8")
        print(f"smoke: {name} (frame {case['frame']})", flush=True)
        try:
            result = subprocess.run(
                command,
                cwd=ROOT,
                env={**smoke_environment(case, image, settings), **extra},
                timeout=120,
                check=False,
            )
        except subprocess.TimeoutExpired:
            print(f"smoke: timed out; partial frame: {image}", file=sys.stderr)
            return False
        if result.returncode != 0 or not image.is_file():
            print(f"smoke: game exited {result.returncode}; differing frame: {image}", file=sys.stderr)
            return False
        actual = digest(image)
        if record:
            case["sha256"] = actual
            fixture.write_text(json.dumps(case, indent=2) + "\n", encoding="utf-8")
            print(f"smoke: recorded {actual}")
        elif actual != case.get("sha256"):
            print(f"smoke: {name} mismatch", file=sys.stderr)
            print(f"  expected {case.get('sha256', '<missing>')}", file=sys.stderr)
            print(f"  actual   {actual}", file=sys.stderr)
            print(f"  differing frame: {image}", file=sys.stderr)
            return False
        else:
            print(f"smoke: {name} passed")
    return True


def check_mod_exports(executable: Path) -> bool:
    """The table code mods bind to matches the link (check_mod_exports.py)."""
    result = subprocess.run([sys.executable, str(ROOT / "tools/pc/check_mod_exports.py"), str(executable)],
                            cwd=ROOT, check=False)
    return result.returncode == 0


def run_ctests(build: Path) -> bool:
    if not (build / "CTestTestfile.cmake").is_file():
        print(f"smoke: CTest build is missing: {build}", file=sys.stderr)
        return False
    result = subprocess.run(
        ["ctest", "--test-dir", str(build), "-R", "^pc_", "--output-on-failure"],
        cwd=ROOT,
        check=False,
    )
    return result.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", action="store_true", help="replace fixture hashes with current output")
    parser.add_argument("--executable", type=Path, default=DEFAULT_EXECUTABLE)
    parser.add_argument("--windows", action="store_true",
                        help=f"test {WINDOWS_EXECUTABLE.relative_to(ROOT)} (under Wine off Windows) and the "
                             "Windows mod loader instead of the CTests")
    parser.add_argument("--build", type=Path, default=DEFAULT_BUILD, help="CTest build directory")
    arguments = parser.parse_args()
    if arguments.windows:
        exports_ok = check_mod_exports(WINDOWS_EXECUTABLE)
        loader_ok = subprocess.run([sys.executable, str(ROOT / "tools/pc/test_object_loader.py"), "--target", "windows"],
                                   cwd=ROOT, check=False).returncode == 0
        return 0 if run_smoke(WINDOWS_EXECUTABLE, arguments.record) and exports_ok and loader_ok else 1
    exports_ok = check_mod_exports(arguments.executable.resolve())
    screenshots_ok = run_smoke(arguments.executable.resolve(), arguments.record) and exports_ok
    tests_ok = run_ctests(arguments.build.resolve())
    return 0 if screenshots_ok and tests_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
