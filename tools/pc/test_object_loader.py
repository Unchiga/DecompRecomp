#!/usr/bin/env python3
"""Test the code mod loader in a 32-bit process on Linux, on Windows, or both.

Builds the fixtures in tests/pc/mod_fixtures with build_mod.py (the good one
as a mod is built; the broken ones with the flag that breaks each), builds
tests/pc/object_loader_test.c for each system, and runs it: natively on
Linux, and under Wine off Windows. The same fixture files are used for both,
which is the point: one object, both systems.

ctest runs the Linux half (pc_object_loader); smoke.py --windows runs the
Windows half."""
import argparse, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_mod

OUT = os.path.join(ROOT, "tmp/pc/object-loader-test")
FIXTURES = os.path.join(ROOT, "tests/pc/mod_fixtures")
SOURCES = ["src/pc/compat/fs.c", "tests/pc/object_loader_test.c", "src/pc/mods/object_loader.c", "src/pc/mods/mod_libc.c"]


def fixtures():
    directory = os.path.join(OUT, "fixtures")
    objects = os.path.join(OUT, "objects")
    source = lambda *names: [os.path.join(FIXTURES, name) for name in names]
    build_mod.compile_object(source("good.c", "good_other.c"), os.path.join(directory, "good.o"), objects)
    for name, files, flags in (("pic", ["good.c", "good_other.c"], ["-fPIC"]),
                               ("unknown", ["unknown.c"], []),
                               ("common", ["common.c"], ["-fcommon"]),
                               ("ctor", ["ctor.c"], []),
                               ("protected", ["protected.c"], ["-fstack-protector-all"])):
        build_mod.compile_object(source(*files), os.path.join(directory, name + ".o"),
                                 os.path.join(objects, name), flags)
    with open(os.path.join(directory, "good.o"), "rb") as handle:
        good = handle.read()
    with open(os.path.join(directory, "truncated.o"), "wb") as handle:
        handle.write(good[:len(good) - 100])   # the section table is at the end
    with open(os.path.join(directory, "garbage.o"), "wb") as handle:
        handle.write(bytes((index * 73 + 41) & 0xff for index in range(4096)))
    open(os.path.join(directory, "empty.o"), "wb").close()
    return directory


def run_test(target, directory):
    flags = ["-std=gnu11", "-O2", "-g", "-Wall", "-Wextra", "-Isrc"]
    if target == "linux":
        program = os.path.join(OUT, "object_loader_test")
        command = ["gcc", "-m32", *flags, *SOURCES, "-lm", "-o", program]
        launch, environment = [program], dict(os.environ)
    else:
        import build_win32_deps
        build_win32_deps.use_toolchain()
        program = os.path.join(OUT, "object_loader_test.exe")
        command = ["i686-w64-mingw32-clang", *flags, *SOURCES, "-static", "-o", program]
        launch, environment = [program], dict(os.environ)
        if sys.platform != "win32":
            launch = ["wine", program]
            environment.update(WINEPREFIX=os.path.join(ROOT, "tmp/pc/wine-prefix"),
                               WINEDLLOVERRIDES="mscoree,mshtml=", WINEDEBUG="-all")
    built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if built.returncode:
        print(f"test_object_loader: building for {target} failed\n{built.stderr}", file=sys.stderr)
        return False
    result = subprocess.run(launch + [directory], cwd=ROOT, env=environment, capture_output=True, text=True,
                            timeout=600)
    output = result.stdout.replace("\r", "")
    print(f"--- {target}\n{output}", end="")
    if result.returncode:
        print(f"test_object_loader: {target} exited {result.returncode}\n{result.stderr}", file=sys.stderr)
        return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("linux", "windows", "both"), default="both")
    options = parser.parse_args()
    os.chdir(ROOT)
    directory = fixtures()
    targets = ["linux", "windows"] if options.target == "both" else [options.target]
    ok = all([run_test(target, directory) for target in targets])
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
