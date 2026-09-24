#!/usr/bin/env python3
"""Build a code mod: every .c in its directory, merged into one <library>.o.

The object runs on both the Linux and the Windows game, which load it with
their own loader (src/pc/mods/object_loader.c), so a mod is built once, on
either system, with the same result. That only holds because the flags below
close every gap between the two 32-bit ABIs (notes/portable-mods-plan.md):

  -fno-pic -fno-common            plain relocations only; no GOT, no COMMON
  -fno-stack-protector            the canary lives in Linux thread storage
  -march=i686 -mno-sse            x87 floating point, no SSE alignment needs
  -mstackrealign                  every function aligns its own stack to 16:
                                  Windows only promises 4 on the way in, and
                                  the Linux game (SSE2) needs 16 on the way
                                  out, or it faults on the first movaps
  -fstack-clash-protection        touches each stack page on the way down,
                                  as Windows' guard page requires
  -ffreestanding -nostdinc        no system C library: the SDK's headers
                                  (src/pc/mods/sdk) declare what the game lends

The compiler is clang (on Windows, llvm-mingw's, targeting i386 Linux ELF),
else gcc -m32. MEMORIES_MOD_CC names another. The game's headers come from
src/ (in this repository) or from the sdk/include the game ships beside it,
where this script is sdk/tools/build_mod.py.

The names the mod leaves undefined are checked against what the game
provides: in the repository, each game build (--game, default both); beside
a game, the list its SDK was shipped with. A mod that would load on one
system and not the other fails here rather than in the game."""
import argparse, glob, json, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# In the repository, or the copy in the sdk/ directory beside a game
# (sdk/tools/build_mod.py, with the headers in sdk/include).
SHIPPED = not os.path.isdir(os.path.join(ROOT, "src/pc/mods/sdk"))
SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLVM_MINGW = os.path.join(ROOT, "tmp/pc/llvm-mingw/bin")
GAME_BUILDS = [SDK] if SHIPPED else [os.path.join(ROOT, "tmp/pc/game32"), os.path.join(ROOT, "tmp/pc/win32")]
FLAGS = ["-std=gnu11", "-O2", "-g", "-fno-pic", "-fno-pie", "-fno-common", "-fno-stack-protector",
         "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fstack-clash-protection",
         "-march=i686", "-mno-sse", "-mno-mmx", "-ffreestanding", "-nostdinc",
         "-DMEMORIES_PC", "-DMEMORIES_MOD", "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Wall",
         "-Wno-unused-function", "-Wno-missing-braces"]
CLANG_FLAGS = ["--target=i386-pc-linux-gnu", "-mstackrealign"]
GCC_FLAGS = ["-m32", "-mstackrealign", "-mincoming-stack-boundary=2"]


def tool(name):
    """A program from PATH, or from the llvm-mingw this repository fetches."""
    found = shutil.which(name)
    if not found and os.path.exists(os.path.join(LLVM_MINGW, name + (".exe" if os.name == "nt" else ""))):
        found = os.path.join(LLVM_MINGW, name)
    return found


def compiler():
    """(command, flags, linker command) for building an i386 ELF object."""
    named = os.environ.get("MEMORIES_MOD_CC")
    candidates = [named] if named else [os.path.join(LLVM_MINGW, "clang"), "clang", "gcc"]
    for candidate in candidates:
        path = candidate if os.path.isabs(candidate) and os.path.exists(candidate) else tool(candidate)
        if not path:
            continue
        clang = "clang" in os.path.basename(path)
        if clang:
            resource = subprocess.run([path, "-print-resource-dir"], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip()
            include = os.path.join(resource, "include")
            # The lld that came with this clang. Windows needs the .exe
            # spelled out: "ld.lld" already has an extension, so it adds none.
            beside = os.path.join(os.path.dirname(path), "ld.lld")
            linker = next((p for p in (beside + ".exe", beside) if os.path.exists(p)), None) or tool("ld.lld")
            if not linker:
                continue  # clang without lld cannot merge the objects; try the next compiler
            return [path], CLANG_FLAGS + ["-isystem", include], [linker, "-r"]
        include = subprocess.run([path, "-m32", "-print-file-name=include"], capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip()
        return [path], GCC_FLAGS + ["-isystem", include], [tool("ld") or "ld", "-m", "elf_i386", "-r"]
    sys.exit("build_mod: no compiler found; install clang or gcc (with 32-bit support), or set MEMORIES_MOD_CC")


def headers():
    """The SDK's C library, then the game's headers."""
    if SHIPPED:
        return ["-isystem", os.path.join(SDK, "include", "libc"), "-I", os.path.join(SDK, "include")]
    return ["-isystem", os.path.join(ROOT, "src/pc/mods/sdk"), "-I", os.path.join(ROOT, "src")]


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode:
        sys.exit(f"build_mod: {' '.join(command[:3])} ... failed\n{result.stdout}{result.stderr}")
    return result.stdout


def provided(build):
    """The names a game build lends mods: its export table and the C
    library list, or None if that build is not there. A game build in the
    repository has its generated table; an SDK beside a game has the list
    it was shipped with (exports.txt)."""
    table, shipped = os.path.join(build, "mod_exports.c"), os.path.join(build, "exports.txt")
    if os.path.exists(table):
        with open(table) as handle:
            names = set(re.findall(r'^    \{"([^"]+)"', handle.read(), re.M))
        return names | libc_names()
    if os.path.exists(shipped):
        with open(shipped) as handle:
            return set(handle.read().split())
    return None


def libc_names():
    """The C library list in src/pc/mods/mod_libc.c."""
    with open(os.path.join(ROOT, "src/pc/mods/mod_libc.c")) as handle:
        return set(re.findall(r"\bF\((\w+)\)", handle.read()))


def library_name(directory):
    """The object's file name, relative to the mod's directory, by the game's
    own rule (read_manifest in src/pc/mods/mods.c): "library" as written
    when it has a '.' anywhere in it, else with ".o" added; the directory's
    name when there is no "library"."""
    manifest = os.path.join(directory, "mod.json")
    name = None
    if os.path.exists(manifest):
        with open(manifest, encoding="utf-8") as handle:
            name = json.load(handle).get("library")
    name = name or os.path.basename(os.path.normpath(directory))
    return name if "." in name else name + ".o"


def compile_object(sources, output, objects_dir, extra_flags=()):
    """Compile `sources` with the mod flags (then `extra_flags`, which the
    loader's tests use to make broken objects) and merge them into `output`."""
    cc, cc_flags, linker = compiler()
    objects = []
    os.makedirs(objects_dir, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    for source in sources:
        obj = os.path.join(objects_dir, os.path.basename(source) + ".o")
        run(cc + FLAGS + cc_flags + headers() + [*extra_flags, "-c", source, "-o", obj])
        objects.append(obj)
    run(linker + ["-o", output] + objects)
    return output


def build(directory, out_dir=None, objects_dir=None, extra_flags=(), games=GAME_BUILDS, quiet=False):
    """Build the mod in `directory`; returns the path of its .o, or None when
    it has no C (a data-only mod). Rebuilds only when a source or header is
    newer than the object."""
    sources = sorted(glob.glob(os.path.join(directory, "*.c")))
    if not sources:
        return None
    name = library_name(directory)
    out_dir = out_dir or directory
    objects_dir = objects_dir or os.path.join(ROOT, "tmp/pc/mod-objects", name)
    output = os.path.join(out_dir, name)
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)   # "library": "sub/rules"
    os.makedirs(objects_dir, exist_ok=True)
    inputs = sources + glob.glob(os.path.join(directory, "*.h")) + [os.path.abspath(__file__)]
    inputs += glob.glob(os.path.join(SDK if SHIPPED else os.path.join(ROOT, "src"), "**/*.h"), recursive=True)
    if os.path.exists(output) and os.path.getmtime(output) >= max(os.path.getmtime(p) for p in inputs):
        return output
    compile_object(sources, output, objects_dir, ["-I", directory, *extra_flags])
    check(output, games)
    if not quiet:
        print(f"{output}: {len(sources)} source{'s' if len(sources) != 1 else ''}")
    return output


def check(output, games):
    """What the object leaves undefined must be there on every game build."""
    reader = tool("llvm-readelf") or tool("readelf")
    undefined = set()
    for line in run([reader, "-sW", output]).splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[6] == "UND" and parts[4] != "WEAK":
            undefined.add(parts[7])
    for bad, why in (("_GLOBAL_OFFSET_TABLE_", "it is position-independent"),
                     ("__stack_chk_fail", "it uses the stack protector"),
                     ("__stack_chk_fail_local", "it uses the stack protector")):
        if bad in undefined:
            sys.exit(f"build_mod: {output}: {why}; build it with the flags in this script")
    for build in games:
        names = provided(build)
        if names is None:
            continue
        missing = sorted(undefined - names)
        if missing:
            os.remove(output)
            sys.exit(f"build_mod: {output} needs names the game at {os.path.relpath(build, ROOT)} "
                     f"does not provide: {' '.join(missing)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", help="the mod's directory (its mod.json and .c files)")
    parser.add_argument("--out", help="where the .o goes (default: the mod's directory)")
    parser.add_argument("--game", action="append",
                        help="a game build directory to check the mod's names against (repeatable; "
                             "default: tmp/pc/game32 and tmp/pc/win32 when they exist)")
    options = parser.parse_args()
    output = build(options.directory, options.out, games=options.game or GAME_BUILDS)
    if not output:
        print(f"{options.directory}: no C sources; a data-only mod needs no build")


if __name__ == "__main__":
    main()
