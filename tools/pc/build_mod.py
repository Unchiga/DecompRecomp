#!/usr/bin/env python3
"""Build a mod's library: one `.mod` file that runs on Linux and on Windows.

    python3 tools/pc/build_mod.py mods/my-mod            # writes mods/my-mod/my-mod.mod
    python3 tools/pc/build_mod.py mods/my-mod --out DIR  # writes DIR/my-mod.mod

A `.mod` is a 32-bit x86 ELF shared object that the game loads with its own
loader (src/pc/mods/modload.c), the same way on every platform. It is
compiled against the C library the game gives mods (src/pc/mods/libc) and the
game's own headers, and linked against nothing: every name it leaves open is
resolved by the game when the mod is applied, from the game itself and from
that C library, and a name in neither refuses the load. See
notes/modding.md, "Native mods".

The toolchain is clang with lld, on Linux or Windows (llvm-mingw ships both),
or failing that GCC with -m32 on Linux. Both make the same file."""
import argparse, glob, json, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIBC = "src/pc/mods/libc"
SUFFIX = ".mod"

COMMON = ["-std=gnu11", "-O2", "-g", "-fPIC", "-ffreestanding", "-fno-stack-protector", "-fno-strict-aliasing",
          "-fvisibility=default", "-Wall", "-DMEMORIES_PC", "-DMEMORIES_MOD", "-D_LANGUAGE_C", "-DLANGUAGE_C"]
# -Bsymbolic: a mod's own definitions win inside it. The sysv hash table is
# what the loader counts symbols by.
LINK = ["-shared", "-nostdlib", "-Wl,-Bsymbolic", "-Wl,--hash-style=both", "-Wl,-z,noexecstack"]


def toolchain():
    """(compile command, link command) for whichever compiler is here."""
    clang = shutil.which("clang")
    lld = shutil.which("ld.lld")
    if clang and lld:
        # The mod's own stack may be only 4-byte aligned when the Windows
        # game calls it, where Linux promises 16: -mstackrealign keeps SSE
        # spills safe either way.
        target = ["--target=i686-unknown-linux-gnu"]
        return ([clang, *target, *COMMON, "-nostdlibinc", "-isystem", f"{ROOT}/{LIBC}", "-mstackrealign",
                 "-Wno-gnu-folding-constant"],
                [clang, *target, "-fuse-ld=lld", *LINK])
    gcc = shutil.which("gcc")
    if gcc and not sys.platform.startswith("win"):
        builtin = subprocess.run([gcc, "-m32", "-print-file-name=include"], capture_output=True,
                                 text=True).stdout.strip()
        return ([gcc, "-m32", *COMMON, "-nostdinc", "-isystem", f"{ROOT}/{LIBC}", "-isystem", builtin,
                 "-mincoming-stack-boundary=2"],
                [gcc, "-m32", *LINK])
    sys.exit("building a mod needs clang and ld.lld (or GCC with -m32 on Linux); see notes/modding.md")


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        sys.exit(" ".join(command[:4]) + " ...\n" + result.stderr)
    if result.stderr.strip():
        sys.stderr.write(result.stderr)


def library_name(source_dir):
    """The manifest's "library", without a suffix, or the directory's name."""
    name = os.path.basename(os.path.normpath(source_dir))
    try:
        with open(os.path.join(source_dir, "mod.json"), encoding="utf-8") as handle:
            name = json.load(handle).get("library") or name
    except (OSError, ValueError):
        pass
    return name[:-len(SUFFIX)] if name.endswith(SUFFIX) else name


def build(source_dir, out_dir=None, obj_dir=None, headers_mtime=0.0):
    """Compile and link the mod in `source_dir` if it has C and anything is
    newer than its library. Returns the library's path, or None for a mod
    that is only data."""
    sources = sorted(glob.glob(os.path.join(source_dir, "*.c")))
    if not sources:
        return None
    out_dir = out_dir or source_dir
    obj_dir = obj_dir or os.path.join(ROOT, "tmp", "mods", os.path.basename(os.path.normpath(source_dir)))
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(obj_dir, exist_ok=True)
    library = os.path.join(out_dir, library_name(source_dir) + SUFFIX)
    newest = max([headers_mtime, os.path.getmtime(os.path.abspath(__file__))] +
                 [os.path.getmtime(path) for path in sources + glob.glob(os.path.join(source_dir, "*.h"))])
    if os.path.exists(library) and os.path.getmtime(library) >= newest:
        return library
    compile_command, link_command = toolchain()
    objects = []
    for source in sources:
        obj = os.path.join(obj_dir, os.path.basename(source) + ".o")
        run([*compile_command, f"-I{ROOT}/src", f"-I{source_dir}", "-c", source, "-o", obj])
        objects.append(obj)
    run([*link_command, "-o", library, *objects])
    return library


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mod", help="the mod's directory (the one with mod.json in it)")
    parser.add_argument("--out", help="where to write the .mod (default: the mod's directory)")
    options = parser.parse_args()
    headers = glob.glob(f"{ROOT}/src/**/*.h", recursive=True)
    library = build(options.mod, options.out, headers_mtime=max(map(os.path.getmtime, headers)))
    print(library or f"{options.mod}: no C sources; nothing to build")


if __name__ == "__main__":
    main()
