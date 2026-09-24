#!/usr/bin/env python3
"""Exercise SDL virtual controllers and the Controls window on Linux/Windows.

Requires the SDL and PNG dependencies used by build_game32.py. Off Windows,
--target windows runs the 32-bit executable through Wine (needs a display).
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ["src/pc/compat/fs.c", "tests/pc/controls_backend_test.c"] + [
    f"src/pc/platform/{name}.c" for name in
    ("controls", "controls_config", "controls_runtime", "controls_art",
     "controls_window", "controls_linux", "paths")]


def run(target, software=False):
    out = ROOT / "tmp/pc/controls-backend"
    out.mkdir(parents=True, exist_ok=True)
    flags = ["-std=gnu11", "-O2", "-ffunction-sections", "-fdata-sections", "-Isrc"]
    env = dict(os.environ)
    if software:
        env.update(MEMORIES_TEST_SOFTWARE="1", SDL_RENDER_DRIVER="software")
    if target == "linux":
        exe = out / "controls-test"
        command = ["cc", "-m32", *flags,
                   "-Itmp/pc/sdl-source/SDL3-3.4.16/include", *SOURCES,
                   "-Wl,--gc-sections", "tmp/pc/sdl-m32-portable/libSDL3.a",
                   "-lpng16", "-lGL", "-ldl", "-lpthread", "-lm", "-o", str(exe)]
        launch = [str(exe)]
        env.setdefault("SDL_VIDEODRIVER", "offscreen")
    else:
        import build_win32_deps
        build_win32_deps.use_toolchain()
        deps = ROOT / "tmp/pc/win32-deps"
        exe = out / "controls-test.exe"
        command = ["i686-w64-mingw32-clang", *flags, f"-I{deps}/include",
                   f"-I{deps}/sdl/include", *SOURCES, "tests/pc/win32_support.c",
                   "tests/pc/controls_backend_unused.c",
                   "-Wl,--gc-sections", "-static", f"{deps}/sdl/lib/libSDL3.dll.a",
                   f"{deps}/lib/libpng16.a", f"{deps}/lib/libzs.a", "-lopengl32",
                   "-lwinpthread", "-lshell32", "-o", str(exe)]
        shutil.copyfile(deps / "sdl/bin/SDL3.dll", out / "SDL3.dll")
        launch = [str(exe)]
        if sys.platform != "win32":
            launch.insert(0, "wine")
            env.update(WINEPREFIX=str(ROOT / "tmp/pc/wine-prefix"), WINEDEBUG="-all",
                       WINEDLLOVERRIDES="mscoree,mshtml=")
    subprocess.run(command, cwd=ROOT, check=True)
    print(f"Testing {target}", flush=True)
    subprocess.run(launch, cwd=ROOT, env=env, check=True, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("linux", "windows", "both"),
                        default="windows" if sys.platform == "win32" else "linux")
    parser.add_argument("--software", action="store_true", help="Use SDL software rendering; skip GL checks")
    args = parser.parse_args()
    for target in ("linux", "windows") if args.target == "both" else (args.target,):
        run(target, args.software)


if __name__ == "__main__":
    main()
