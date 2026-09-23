#!/usr/bin/env python3
"""Check that 32-bit Linux and 32-bit Windows lay the game's structures out alike.

Both executables share one memory image with the retail game, and a code mod
is one object file for both (notes/portable-mods-plan.md), so every structure
in the headers must have the same size, alignment and field offsets on both.
They can differ: i386 Linux aligns `long long` and `double` to 4 inside a
structure and MinGW aligns them to 8 (as MIPS does), and MinGW packs
bitfields as MSVC does unless built with -mno-ms-bitfields.

Each header under src/ is compiled on its own by the same clang for
i386-pc-linux-gnu and for i686-w64-mingw32 (with the Windows build's
-mno-ms-bitfields), with clang's record layout dump, and the two dumps are
compared structure by structure. A header that does not compile on its own is
reported and skipped; one whose structures differ fails the check.

src/pc/platform is left out: window, settings and controls state, which is
never in guest memory, never saved as bytes, and not part of what a mod
builds against."""
import argparse, concurrent.futures, glob, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLANG = os.path.join(ROOT, "tmp/pc/llvm-mingw/bin/clang")
FLAGS = ["-std=gnu11", "-fsyntax-only", "-w", "-DMEMORIES_PC", "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Isrc",
         "-Xclang", "-fdump-record-layouts-complete", "-Xclang", "-fdump-record-layouts-simple"]
# The psyq headers expect the SDK's order (libgte.h, libgpu.h, libgs.h) and find one
# another with <angled> includes; a header that fails alone is tried again so.
PRELUDE = ["-isystem", "src/psyq", "-include", "src/psyq/libgte.h", "-include", "src/psyq/libgpu.h",
           "-include", "src/psyq/libgs.h"]
TARGETS = {"linux": ["--target=i386-pc-linux-gnu"], "windows": ["--target=i686-w64-mingw32", "-mno-ms-bitfields"]}


TAGS = None  # struct and union tags defined under src/
PORT_PRIVATE = os.path.join("src", "pc", "platform") + os.sep


def ours(name):
    """Whether a record is declared under src/ rather than in a system header,
    and the name to compare it by: an unnamed one's position, with the path
    spelled one way however it was reached."""
    unnamed = re.search(r"\(unnamed at ([^:]+)(:\d+:\d+)\)", name)
    if unnamed:
        path = os.path.relpath(os.path.normpath(os.path.join(ROOT, unnamed.group(1))), ROOT)
        if not path.startswith("src" + os.sep) or path.startswith(PORT_PRIVATE):
            return None
        return name.replace(unnamed.group(0), f"(unnamed at {path}{unnamed.group(2)})")
    tag = name.split("::")[0].split()
    return name if len(tag) == 2 and tag[1] in TAGS else None


def layouts(header, target):
    """{record: its layout text} from one header, or None if it does not compile."""
    for prelude in ([], PRELUDE):
        result = subprocess.run([CLANG, *TARGETS[target], *FLAGS, *prelude, "-include", header, "-x", "c", os.devnull],
                                cwd=ROOT, capture_output=True, text=True)
        if not result.returncode:
            break
    else:
        return None
    found, name, lines = {}, None, []
    for line in result.stdout.splitlines():
        if line.startswith("*** Dumping AST Record Layout"):
            if name:
                found[name] = "\n".join(lines)
            name, lines = None, []
        elif line.startswith("Type: "):
            name = ours(line[len("Type: "):])
        elif name and line.strip() and not line.startswith("Layout: <ASTRecordLayout"):
            lines.append(line.strip())
    if name:
        found[name] = "\n".join(lines)
    return found


def check(header):
    linux, windows = layouts(header, "linux"), layouts(header, "windows")
    if linux is None or windows is None:
        return header, None, []
    # A record only one side has is port code behind #ifdef _WIN32.
    return header, len(linux), [(name, linux[name], windows[name]) for name in sorted(linux)
                                if name in windows and linux[name] != windows[name]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("headers", nargs="*", help="headers to check (default: every one under src/)")
    parser.add_argument("--verbose", action="store_true", help="show both layouts of every difference")
    options = parser.parse_args()
    if not os.path.exists(CLANG):
        sys.exit(f"{CLANG} is missing; run tools/pc/build_win32_deps.py first")
    global TAGS
    TAGS = set()
    for path in glob.glob("src/**/*.h", recursive=True, root_dir=ROOT):
        if os.path.normpath(path).startswith(PORT_PRIVATE):
            continue
        with open(os.path.join(ROOT, path), errors="replace") as handle:
            TAGS.update(re.findall(r"\b(?:struct|union)\s+(\w+)\s*\{", handle.read()))
    headers = options.headers or sorted(path for path in glob.glob("src/**/*.h", recursive=True, root_dir=ROOT)
                                        if not os.path.normpath(path).startswith(PORT_PRIVATE))
    skipped, records, differing = [], set(), {}
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for header, count, differences in pool.map(check, headers):
            if count is None:
                skipped.append(header)
                continue
            records.add(header)
            for name, linux, windows in differences:
                differing.setdefault(name, (header, linux, windows))
    for name, (header, linux, windows) in sorted(differing.items()):
        print(f"{name} ({header}) differs between Linux and Windows")
        if options.verbose:
            print("  linux:\n    " + linux.replace("\n", "\n    "))
            print("  windows:\n    " + windows.replace("\n", "\n    "))
    print(f"check_layouts: {len(records)} headers compared, {len(skipped)} do not compile alone, "
          f"{len(differing)} structures differ")
    if skipped and options.verbose:
        print("  not compiled: " + " ".join(skipped))
    return 1 if differing else 0


if __name__ == "__main__":
    raise SystemExit(main())
