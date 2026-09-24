#!/usr/bin/env python3
"""Run the shipped AI planner and i386 adapter fixtures on Linux/Windows."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

import build_win32_deps

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tmp/pc/ai-hard-mode-tests"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("linux", "windows", "both"), default="linux")
    parser.add_argument("--sanitize", action="store_true", help="Enable ASan/UBSan for Linux fixtures")
    args = parser.parse_args()
    os.chdir(ROOT)
    OUT.mkdir(parents=True, exist_ok=True)
    targets = ("linux", "windows") if args.target == "both" else (args.target,)
    for target in targets:
        environment = dict(os.environ, TMPDIR=str(OUT))
        flags = ["-std=gnu11", "-O1", "-g", "-DMEMORIES_PC", "-Isrc", "-Imods/ai-hard-mode"]
        if target == "linux":
            compiler = ["gcc", "-m32"]
            flags += ["-Wall", "-Wextra"]
            if args.sanitize:
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            launcher = []
        else:
            build_win32_deps.use_toolchain()
            compiler = ["i686-w64-mingw32-clang"]
            flags += ["-static", "-mno-ms-bitfields", "-Wno-gnu-folding-constant"]
            launcher = [] if sys.platform == "win32" else ["wine"]
            environment = dict(os.environ, TMPDIR=str(OUT))
            environment.update(WINEPREFIX=str(ROOT / "tmp/pc/wine-prefix"),
                               WINEDLLOVERRIDES="mscoree,mshtml=", WINEDEBUG="-all")
        for fixture in ("ai_hard_mode_test", "ai_hard_mode_adapter_test"):
            program = OUT / (fixture + (".exe" if target == "windows" else ""))
            subprocess.run([*compiler, *flags, f"tests/pc/{fixture}.c",
                            "mods/ai-hard-mode/planner.c", "-o", str(program)], check=True)
            subprocess.run([*launcher, str(program)], env=environment, check=True, timeout=60)
            print(f"AI hard mode: {target} {fixture} passed", flush=True)


if __name__ == "__main__":
    main()
