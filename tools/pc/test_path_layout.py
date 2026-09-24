#!/usr/bin/env python3
"""Run PC CTests with Unicode temp paths and check a relocated executable/launcher."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NAME = "Jos\u00e9-e\u0301-\u042f-\u6771\u4eac-\U0001f600 & (100%) ! ^"


def batch_launcher(folder, env):
    """Force play.bat's embedded-Python path without downloading/building."""
    (folder / "game").mkdir()
    (folder / "game/disc.bin").touch()
    python = folder / "tmp/pc/tools/python/python.exe"
    game = folder / "tmp/pc/game32/memories-pc.exe"
    python.parent.mkdir(parents=True)
    game.parent.mkdir(parents=True)
    source = folder / "launcher_probe.c"
    source.write_text('''#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    FILE *f;
    if (argc > 1 && !strcmp(argv[1], "-c")) return 1;
    f = fopen(argc > 1 ? "built.marker" : "launched.marker", "wb");
    if (!f) return 2;
    return fclose(f);
}
''', encoding="utf-8")
    subprocess.run(["i686-w64-mingw32-clang", str(source), "-static", "-o", str(python)], check=True)
    shutil.copy2(python, game)
    shutil.copy2(ROOT / "play.bat", folder / "play.bat")
    env = dict(env, PATH=str(python.parent) + os.pathsep + env["PATH"])
    subprocess.run(["cmd.exe", "/d", "/v:on", "/c", "play.bat"], cwd=folder, env=env,
                   stdin=subprocess.DEVNULL, check=True, timeout=30)
    assert (folder / "built.marker").is_file()
    assert (folder / "launched.marker").is_file()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    with tempfile.TemporaryDirectory(prefix="memories-paths-") as temp:
        folder = Path(temp) / NAME
        folder.mkdir()
        env = dict(os.environ, TMPDIR=str(folder), TEMP=str(folder), TMP=str(folder))
        subprocess.run(["ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure"],
                       env=env, check=True)
        exe = "memories_fs_test.exe" if sys.platform == "win32" else "memories_fs_test"
        copy = folder / exe
        shutil.copy2(build / exe, copy)
        subprocess.run([str(copy), NAME], cwd=folder, env=env, check=True, timeout=30)
        if sys.platform == "win32":
            batch_launcher(folder, env)
    print("Unicode executable, temp paths, and launcher: passed")


if __name__ == "__main__":
    main()
