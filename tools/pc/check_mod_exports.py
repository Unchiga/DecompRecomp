#!/usr/bin/env python3
"""Check the table code mods bind to (src/pc/mods/exports.h) against the link.

Runs the executable with MEMORIES_MOD_EXPORTS=1 (under Wine for the Windows
one off Windows), which prints the table and exits, then checks that it is
sorted as strcmp sorts, has no repeats, and that every name the executable's
own symbol listing also has sits at the address the linker gave it. A game
function, a pinned guest variable and a port variable must all be there:
those are the three kinds of name a mod reaches (3D Monsters uses each)."""
import argparse, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REQUIRED = ("Duel_DrawFieldCards", "D_800E9DB0", "SoftGpu_Bank")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?", default=os.path.join(ROOT, "tmp/pc/game32/memories-pc"))
    options = parser.parse_args()
    windows = options.executable.endswith(".exe")
    environment = dict(os.environ, MEMORIES_MOD_EXPORTS="1")
    command = [options.executable]
    if windows and sys.platform != "win32":
        command = ["wine"] + command
        environment.update(WINEPREFIX=os.path.join(ROOT, "tmp/pc/wine-prefix"),
                           WINEDLLOVERRIDES="mscoree,mshtml=", WINEDEBUG="-all")
    result = subprocess.run(command, env=environment, capture_output=True, text=True)
    if result.returncode:
        sys.exit(f"{options.executable} exited {result.returncode}\n{result.stderr}")
    table = [line.split() for line in result.stdout.splitlines() if line.strip()]
    names = [row[1] for row in table]
    problems = []
    if names != sorted(set(names), key=lambda name: name.encode()):
        problems.append("the table is not sorted, or repeats a name")
    nm = "llvm-nm" if windows else "nm"
    if windows and sys.platform != "win32":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import build_win32_deps
        build_win32_deps.use_toolchain()
    linked = {}
    for line in subprocess.run([nm, options.executable], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            name = parts[2][1:] if windows and parts[2].startswith("_") else parts[2]
            linked.setdefault(name, int(parts[0], 16))
    checked = 0
    for address, name in table:
        if name in linked:
            checked += 1
            if linked[name] != int(address, 16):
                problems.append(f"{name}: the table says {address}, the link {linked[name]:08x}")
    for name in REQUIRED:
        if name not in names:
            problems.append(f"{name} is missing")
        elif name not in linked:
            problems.append(f"{name} is not in the executable's symbols, so its address went unchecked")
    for problem in problems[:20]:
        print(f"check_mod_exports: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"{options.executable}: {len(names)} mod exports, {checked} checked against the link")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
