#!/usr/bin/env python3
"""Regression for rejected native-mod callbacks, with one object on both OSes."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import build_mod
import build_win32_deps

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tmp/pc/mods-lifecycle"
SOURCES = ["tests/pc/mods_lifecycle_test.c", "src/pc/mods/mods.c", "src/pc/mods/events.c",
           "src/pc/mods/manager.c", "src/pc/mods/mod_libc.c", "src/pc/mods/object_loader.c",
           "src/pc/mods/json.c", "src/pc/platform/settings.c", "src/pc/platform/paths.c"]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("linux", "windows", "both"), default="linux")
    args = parser.parse_args()
    os.chdir(ROOT)
    OUT.mkdir(parents=True, exist_ok=True)
    os.environ["TMPDIR"] = str(OUT)
    fixture = OUT / "rejected.o"
    build_mod.compile_object(["tests/pc/mod_fixtures/rejected.c"], str(fixture), str(OUT / "obj"))
    no_api = OUT / "no_api.o"
    build_mod.compile_object(["tests/pc/mod_fixtures/no_api.c"], str(no_api), str(OUT / "obj-no-api"))
    for target in ("linux", "windows") if args.target == "both" else (args.target,):
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Isrc"]
        environment = dict(os.environ, TMPDIR=str(OUT))
        if target == "linux":
            program = OUT / "lifecycle"
            compile_cmd = ["gcc", "-m32", *flags, *SOURCES, "-lm", "-o", str(program)]
            launch = [str(program)]
        else:
            build_win32_deps.use_toolchain()
            program = OUT / "lifecycle.exe"
            compile_cmd = ["i686-w64-mingw32-clang", *flags, *SOURCES, "-static", "-lshell32", "-lole32", "-lwinpthread", "-o", str(program)]
            launch = [str(program)]
            if sys.platform != "win32":
                launch = ["wine", str(program)]
                environment.update(WINEPREFIX=str(ROOT / "tmp/pc/wine-prefix"), WINEDLLOVERRIDES="mscoree,mshtml=", WINEDEBUG="-all")
        subprocess.run(compile_cmd, check=True)
        subprocess.run([*launch, str(fixture), str(no_api)], env=environment, check=True, timeout=60)
        print(f"mods lifecycle: {target} passed")
        if target == "linux":
            state_program = str(program) + "-state"
            state_command = [*compile_cmd[:compile_cmd.index(SOURCES[0])], "-ffunction-sections", "-fdata-sections",
                             "tests/pc/mods_state_test.c", "-Wl,--gc-sections", "-o", state_program]
            subprocess.run(state_command, check=True)
            subprocess.run([state_program, str(OUT / "state-chunks.bin")], env=environment, check=True, timeout=60)
            print("mod save-state compatibility: linux passed")

if __name__ == "__main__":
    main()
