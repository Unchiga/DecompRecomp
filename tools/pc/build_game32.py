#!/usr/bin/env python3
"""Compile and link the resident game C as a 32-bit Linux or Windows executable.

Bring-up driver for the fixed-address memory model (src/pc/guest/image.h):
  * every game unit is compiled with the host GCC as ILP32;
  * data symbols the units leave undefined or tentative are pinned to their
    retail addresses, read from the matching build's ELF;
  * undefined functions get generated stubs that name themselves and exit,
    unless a native source under src/pc already defines them.
Requires the matching build's ELF (make match) for symbol addresses.

On Windows the toolchain is llvm-mingw (i686-w64-mingw32-clang, lld and the
llvm binutils) and the libraries come from tools/pc/build_win32_deps.py. PE
differs from ELF in ways the link below works around: C symbols carry a
leading underscore; sections cannot be placed at chosen addresses, so the
fixed game sections (save states across rebuilds) are not available; the
section renames edit the COFF headers directly (rename_coff_sections) and
__start_/__stop_ come from grouped marker sections; overrides win by link order instead of weakened symbols."""
import argparse, concurrent.futures, csv, glob, hashlib, json, os, shutil, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ELF = "tmp/project-build/SLUS_014.11.elf"
# --target windows on Linux cross-compiles with the llvm-mingw that
# build_win32_deps.py fetches; the rest of this file only asks WINDOWS. Read
# before argparse because the flags and tools below depend on it.
TARGET = next((sys.argv[i + 1] for i, word in enumerate(sys.argv[:-1]) if word == "--target"),
              os.environ.get("MEMORIES_TARGET") or ("windows" if sys.platform == "win32" else "linux"))
WINDOWS = TARGET == "windows"
WIN32_DEPS = "tmp/pc/win32-deps"  # tools/pc/build_win32_deps.py
MOD_IMPLIB = "libmemories-pc.a"  # Windows: the executable's import library, which mod DLLs link against
CC, OBJCOPY, NM, READELF, OBJDUMP = (("i686-w64-mingw32-clang", "llvm-objcopy", "llvm-nm", "llvm-readelf", "llvm-objdump")
                                     if WINDOWS else ("gcc", "objcopy", "nm", "readelf", "objdump"))
PREFIX = "_" if WINDOWS else ""  # C symbol names in the object files
if WINDOWS and sys.platform != "win32":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import build_win32_deps
    build_win32_deps.use_toolchain()
if sys.platform == "win32":
    # The paths below are written, compared and turned into object names
    # with forward slashes; Windows glob returns backslashes.
    _glob = glob.glob
    glob.glob = lambda *args, **kwargs: [path.replace(os.sep, "/") for path in _glob(*args, **kwargs)]
CFLAGS = ["-m32", "-std=gnu11", "-fpermissive", "-w", "-O0", "-g", "-fno-strict-aliasing",
          "-fwrapv", "-fcommon", "-fno-pie", "-fno-stack-protector", "-DMEMORIES_PC",
          "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Isrc"]
if WINDOWS:
    # -fpermissive is GCC's; clang needs this one of its errors turned off.
    # -mno-ms-bitfields: MinGW lays bitfields out as MSVC does, where fields
    # of different declared types do not share a unit; the game's layouts are
    # GCC's (GsOT_TAG's `unsigned p:24; unsigned char num:8` is 4 bytes, not
    # 8, or every LIBGS ordering table has the wrong stride).
    CFLAGS = [f for f in CFLAGS if f not in ("-m32", "-fno-pie")] + ["-Wno-incompatible-pointer-types", "-mno-ms-bitfields"]
# -O0 for game units: original busy-waits poll non-volatile globals that the
# VBlank handler updates, and must not be hoisted out of their loops.
NATIVE_CFLAGS = ["-m32", "-std=gnu11", "-O2", "-g", "-Wall", "-fno-pie", "-fno-omit-frame-pointer", "-fno-strict-aliasing",
                 "-Wno-builtin-declaration-mismatch", "-DMEMORIES_PC", "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Isrc",
                 "-I/usr/include/freetype2"]
if WINDOWS:
    NATIVE_CFLAGS = [f for f in NATIVE_CFLAGS if f not in ("-m32", "-fno-pie", "-I/usr/include/freetype2",
                                                           "-Wno-builtin-declaration-mismatch")] + [
        f"-I{WIN32_DEPS}/sdl/include", f"-I{WIN32_DEPS}/include", f"-I{WIN32_DEPS}/include/freetype2",
        "-mno-ms-bitfields"]  # the game's structures, shared with native code (see CFLAGS)
# Window backends (src/pc/platform): SDL3 when its 32-bit static build exists
# (see notes/pc-build.md), else X11. --backend or MEMORIES_BACKEND picks.
SDL_BUILD = "tmp/pc/sdl-m32"
SDL_SOURCE = "tmp/port-research/psyz/external/SDL"
BACKENDS = {"sdl": ["src/pc/platform/sdl.c"],
            "x11": ["src/pc/platform/x11.c", "src/pc/platform/audio_alsa.c", "src/pc/platform/gamepad_evdev.c"]}
BACKEND_SOURCES = sorted(sum(BACKENDS.values(), []))
NATIVE = sorted(glob.glob("src/pc/guest/*.[cS]") + glob.glob("src/pc/sdk/*.c") +
                [f for f in glob.glob("src/pc/platform/*.c") if f not in BACKEND_SOURCES] + glob.glob("src/pc/overlays/*.c") + glob.glob("src/pc/overrides/*.c") + glob.glob("src/pc/audio/*.c") + glob.glob("src/pc/mods/*.c") + glob.glob("src/pc/debug/*.c") + ["src/pc/render/soft_gpu.c"]) + [
    "src/pc/rng.c", "src/pc/compat/gte.c", "src/pc/compat/libgs_ot.c", "src/pc/render/packets.c"]
# Same contract as the host C library, so the host's version is used directly.
# Runtime-loaded modules linked into the executable: name, sources, identifier
# word at the start of the image, and load bank. main_menu has its load address
# (0x80180000) to itself, so it is linked like resident code (bank 0). The
# 0x80168000 modules replace one another there; src/pc/guest/modules.c
# explains what that takes. name_entry is an entry into the password image.
# The overworld's two packages (before and after the coup) are one program:
# their images differ only in the data blob behind the C, which stays in guest
# memory, so one module serves both, configured from the first.
MODULES = [("main_menu", "src/overlays/main_menu/*.c", 0x0F, 0),
           ("password", "src/overlays/password/*.c", 0x15, 0x80168000),
           ("overworld", "src/overlays/overworld/*.c", 0x14, 0x80168000),
           ("free_duel", "src/overlays/free_duel/*.c", 0x13, 0x80168000)]
MODULE_CONFIG = {"overworld": "overworld_before_coup"}

# Save states outlive native rebuilds because everything a state can point at
# in the game objects stays put (src/pc/guest/state.h): their code and
# variables are collected into sections linked at these addresses.
FIXED_SECTIONS = {"game_text": 0x01000000, "game_rodata": 0x03000000, "game_data": 0x04000000,
                  "game_bss": 0x05000000}
MODULE_SECTIONS = 0x06000000  # then 0x00400000 per module: data, and bss 0x00200000 above it

HOST_LIBC = {"printf", "sprintf", "strcmp", "strcpy", "bzero", "qsort", "memcpy", "memset",
             "memmove", "strlen", "strcat", "strncmp", "strncpy", "memcmp"}

def c_name(symbol):
    """The C name of an object-file symbol, or None for toolchain symbols."""
    if not WINDOWS:
        return symbol
    return symbol[1:] if symbol.startswith("_") else None

def run(command):
    response = None
    if sum(len(word) + 1 for word in command) > 30000:
        # Windows' command line holds 32 K, which the object lists reach.
        # The llvm tools, gcc, clang and binutils all read @file arguments.
        os.makedirs("tmp/pc", exist_ok=True)
        with tempfile.NamedTemporaryFile("w", dir="tmp/pc", suffix=".rsp", delete=False) as handle:
            handle.writelines('"%s"\n' % word.replace("\\", "\\\\").replace('"', '\\"') for word in command[1:])
            response = handle.name
        shown, command = command, [command[0], "@" + response]
    else:
        shown = command
    result = subprocess.run(command, capture_output=True, text=True)
    if response:
        os.remove(response)
    if result.returncode:
        sys.exit(f"{' '.join(shown[:6])} ...\n{result.stderr}")
    return result.stdout

def compile_unit(job):
    source, obj, flags, renames = job
    if os.path.exists(obj) and os.path.getmtime(obj) >= NEWEST_HEADER and \
            os.path.getmtime(obj) >= os.path.getmtime(source):
        return
    run([CC, *flags, "-c", source, "-o", obj])
    if WINDOWS:
        # asm("name") labels in the sources name C symbols, which COFF spells
        # with a leading underscore; everything else from C already has one.
        labels = {line.split()[-1] for line in run([NM, "-g", obj]).splitlines()
                  if line.split() and line.split()[-1][:1].isalpha()}
        if labels:
            with open(obj + ".labels", "w") as handle:
                handle.writelines(f"{name} _{name}\n" for name in sorted(labels))
            run([OBJCOPY, f"--redefine-syms={obj}.labels", obj])
    if renames:
        run([OBJCOPY, f"--redefine-syms={renames}", obj])

def symbols(objects):
    defined, tentative, undefined = set(), set(), set()
    for line in run([NM, "-g", *objects]).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] in "UCTDBRVW" and not line.endswith(":") and c_name(parts[-1]):
            {"U": undefined, "C": tentative}.get(parts[-2], defined).add(c_name(parts[-1]))
    return defined, tentative, undefined

def rename_coff_sections(path, renames):
    """objcopy --rename-section for COFF objects, which llvm-objcopy lacks.
    The contents get a $m suffix on Windows: lld sorts a section's $-suffixed
    parts by suffix, which puts them between the $a and $z markers that stand
    in for ELF's __start_ and __stop_ symbols."""
    with open(path, "rb") as handle:
        data = bytearray(handle.read())
    _, count, _, symbols_at, symbol_count, optional_size, _ = struct.unpack_from("<HHIIIHH", data, 0)
    strings_at = symbols_at + symbol_count * 18
    strings_size = struct.unpack_from("<I", data, strings_at)[0]
    if strings_at + strings_size != len(data):
        sys.exit(f"{path}: the string table does not end the file")
    extra = bytearray()
    for index in range(count):
        at = 20 + optional_size + index * 40
        field = bytes(data[at:at + 8]).rstrip(b"\0")
        if field.startswith(b"/"):
            start = strings_at + int(field[1:])
            field = bytes(data[start:data.index(b"\0", start)])
        new = renames.get(field.decode())
        if new is None:
            continue
        encoded = new.encode()
        if len(encoded) > 8:
            reference = f"/{strings_size + len(extra)}".encode()
            extra += encoded + b"\0"
            encoded = reference
        data[at:at + 8] = encoded.ljust(8, b"\0")
    if extra:
        data += extra
        struct.pack_into("<I", data, strings_at, strings_size + len(extra))
    with open(path, "wb") as handle:
        handle.write(data)

def unset_coff_commons(path, names):
    """Turn COMMON symbols (tentative definitions) named in `names` into
    undefined references: lld prefers a COMMON over an absolute definition,
    where a GNU linker script assignment overrides it. A COFF COMMON symbol
    is an external one in no section whose value is its size."""
    with open(path, "rb") as handle:
        data = bytearray(handle.read())
    _, _, _, symbols_at, symbol_count, _, _ = struct.unpack_from("<HHIIIHH", data, 0)
    strings_at = symbols_at + symbol_count * 18
    changed, index = False, 0
    while index < symbol_count:
        at = symbols_at + index * 18
        value, section, _, storage, auxiliary = struct.unpack_from("<IhHBB", data, at + 8)
        if section == 0 and value and storage == 2:  # IMAGE_SYM_CLASS_EXTERNAL
            raw = bytes(data[at:at + 8])
            if raw[:4] == b"\0\0\0\0":
                start = strings_at + struct.unpack_from("<I", raw, 4)[0]
                raw = bytes(data[start:data.index(b"\0", start)])
            if c_name(raw.rstrip(b"\0").decode()) in names:
                struct.pack_into("<I", data, at + 8, 0)
                changed = True
        index += 1 + auxiliary
    if changed:
        with open(path, "wb") as handle:
            handle.write(data)

def definitions(objects):
    """How many of the objects define each name."""
    counts, current = {}, None
    for line in run([NM, "-g", "--defined-only", *objects]).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] in "TDBRV" and c_name(parts[-1]):
            counts[c_name(parts[-1])] = counts.get(c_name(parts[-1]), 0) + 1
    return counts

def build_mods(build):
    """Each directory under mods/ becomes a mod directory beside the game.

    A mod is its manifest and whatever it ships; if it has C, that is linked
    into one shared library named after the directory, which the game dlopens
    when the mod is applied (src/pc/mods/mods.c). Mods resolve the game's and
    the port's symbols out of the executable at load time, so they are built
    against the same headers and nothing else."""
    out_root = f"{build}/mods"
    os.makedirs(out_root, exist_ok=True)
    # The header a mod author builds against, beside the game rather than
    # only in the source tree, so a release carries it (notes/modding.md).
    os.makedirs(f"{build}/include/pc/mods", exist_ok=True)
    with open("src/pc/mods/modapi.h", "rb") as source:
        header = source.read()
    with open(f"{build}/include/pc/mods/modapi.h", "wb") as copy:
        copy.write(header)
    built = []
    for manifest in sorted(glob.glob("mods/*/mod.json")):
        source_dir = os.path.dirname(manifest)
        name = os.path.basename(source_dir)
        out_dir = f"{out_root}/{name}"
        os.makedirs(out_dir, exist_ok=True)
        for path in sorted(glob.glob(f"{source_dir}/**/*", recursive=True)):
            if path.endswith(".c") or path.endswith(".h") or os.path.isdir(path):
                continue
            destination = os.path.join(out_dir, os.path.relpath(path, source_dir))
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            if not os.path.exists(destination) or os.path.getmtime(destination) < os.path.getmtime(path):
                with open(path, "rb") as source, open(destination, "wb") as copy:
                    copy.write(source.read())
        sources = sorted(glob.glob(f"{source_dir}/*.c"))
        if not sources:
            built.append(f"{name} (data)")
            continue
        library = f"{out_dir}/{name}" + (".dll" if WINDOWS else ".so")
        objects = []
        for source in sources:
            obj = f"{build}/obj/{source.replace('/', '_')}.o"
            objects.append(obj)
            compile_unit((source, obj, NATIVE_CFLAGS + ([] if WINDOWS else ["-fPIC"]), None))
        if not os.path.exists(library) or max(os.path.getmtime(o) for o in objects) > os.path.getmtime(library):
            if WINDOWS:
                # A DLL's references to the game and the port resolve through
                # the executable's import library (the link step above); the
                # loader binds them when mods.c loads the DLL. Pinned guest
                # variables are absolute symbols, which a PE cannot export;
                # they sit at the same address in every module, so the DLL
                # links the executable's pins itself. clock_gettime and the
                # like come from winpthreads, static as in the executable.
                run([CC, "-shared", "-o", library, f"{build}/guest_symbols.o", *objects,
                     f"{build}/{MOD_IMPLIB}", "-lm", "-static", "-lpthread"])
            else:
                run(["gcc", "-m32", "-shared", "-o", library, *objects, "-lm"])
        built.append(name)
    if built:
        print(f"{out_root}: " + ", ".join(built))


def main():
    global NEWEST_HEADER
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=list(BACKENDS), default=os.environ.get("MEMORIES_BACKEND") or
                        ("sdl" if WINDOWS or os.path.exists(f"{SDL_BUILD}/libSDL3.a") else "x11"))
    parser.add_argument("--target", choices=("linux", "windows"), default=TARGET)
    # A Windows build made on Linux gets a directory of its own, so both
    # executables and their objects sit side by side.
    parser.add_argument("--build", default="tmp/pc/win32" if WINDOWS and sys.platform != "win32" else "tmp/pc/game32")
    options = parser.parse_args()
    NATIVE.extend(BACKENDS[options.backend])
    NATIVE.sort()
    if WINDOWS:
        if options.backend != "sdl":
            sys.exit("Windows builds use the SDL backend")
        if not os.path.exists(f"{WIN32_DEPS}/lib/libfreetype.a") or not shutil.which(CC):
            sys.exit(f"{WIN32_DEPS} is missing; run tools/pc/build_win32_deps.py first")
    elif options.backend == "sdl":
        if not os.path.exists(f"{SDL_BUILD}/libSDL3.a"):
            sys.exit(f"{SDL_BUILD}/libSDL3.a is missing; build SDL3 for -m32 first (notes/pc-build.md)")
        NATIVE_CFLAGS.extend([f"-I{SDL_SOURCE}/include", f"-I{SDL_BUILD}/include-revision"])
    os.chdir(ROOT)
    if not os.path.exists(ELF):
        sys.exit(f"{ELF} is missing; run `make match` first")
    os.makedirs(options.build + "/obj", exist_ok=True)
    headers = glob.glob("src/**/*.h", recursive=True) + glob.glob("mods/**/*.h", recursive=True) + [__file__, "config/pc/host_symbol_renames.txt"]
    NEWEST_HEADER = max(os.path.getmtime(path) for path in headers)
    obj = lambda source: f"{options.build}/obj/{source.replace('/', '_')}.o"
    # main_menu is the only overlay with a private load address (0x80180000),
    # so it can simply be linked in. The 0x80168000 modules share one address
    # and need a loaded-module registry first.
    resident = sorted(glob.glob("src/game/*.c"))
    module_sources = {name: sorted(glob.glob(pattern)) for name, pattern, _, _ in MODULES}
    game = resident + [source for name, _, _, _ in MODULES for source in module_sources[name]]
    renames_file = "config/pc/host_symbol_renames.txt"
    if WINDOWS:
        with open(renames_file) as handle, open(f"{options.build}/host_symbol_renames.txt", "w") as out:
            for line in handle:
                if line.split():
                    out.write(" ".join(PREFIX + name for name in line.split()) + "\n")
        renames_file = f"{options.build}/host_symbol_renames.txt"
    jobs = [(s, obj(s), CFLAGS, renames_file) for s in game]
    jobs += [(s, obj(s), NATIVE_CFLAGS, None) for s in NATIVE]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        list(pool.map(compile_unit, jobs))

    # Shared-bank modules: a symbol that another module or the resident image
    # also defines, or that their ELFs place at a different address, becomes
    # <module>__<name> inside that module. Their variables move to sections
    # of their own so the registry can reinitialize them on every load.
    def elf_addresses(elf):
        if not os.path.exists(elf):
            sys.exit(f"{elf} is missing; run `make match match-overlays` first")
        found = {}
        for line in run([READELF, "-sW", elf]).splitlines():
            parts = line.split()
            if len(parts) == 8 and parts[4] == "GLOBAL" and parts[6] != "UND":
                found.setdefault(parts[7], int(parts[1], 16))
        return found
    module_elf = {name: elf_addresses("tmp/overlays/{0}/build/{0}.elf".format(MODULE_CONFIG.get(name, name))) for name, _, _, _ in MODULES}
    resident_elf = elf_addresses(ELF)
    module_symbols = {name: symbols([obj(s) for s in module_sources[name]]) for name, _, _, _ in MODULES}
    resident_defined = symbols([obj(s) for s in resident])[0]
    renamed, sections = {}, {}
    for name, _, _, bank in MODULES:
        if not bank:
            continue
        defined, common, wanted_here = module_symbols[name]
        others = [other for other, _, _, _ in MODULES if other != name]
        clash = {symbol for symbol in defined | common
                 if symbol in resident_defined or any(symbol in module_symbols[o][0] | module_symbols[o][1] for o in others)}
        clash |= {symbol for symbol in (wanted_here | common) - defined
                  if symbol in module_elf[name] and symbol not in resident_defined and any(
                      places.get(symbol, module_elf[name][symbol]) != module_elf[name][symbol]
                      for places in [resident_elf] + [module_elf[o] for o in others])}
        renamed[name] = {symbol: f"{name}__{symbol}" for symbol in clash if not symbol.startswith(f"{name}__")}
        for source in module_sources[name]:
            command = [OBJCOPY]
            for old, new in sorted(renamed[name].items()):
                command.append(f"--redefine-sym={PREFIX}{old}={PREFIX}{new}")
            if WINDOWS:
                rename_coff_sections(obj(source), {".data": f"ovl_{name}_data$m", ".bss": f"ovl_{name}_bss$m"})
            else:
                for section in (".data", ".sdata"):
                    command.append(f"--rename-section={section}=ovl_{name}_data")
                for section in (".bss", ".sbss"):
                    command.append(f"--rename-section={section}=ovl_{name}_bss")
            if len(command) > 1:
                run(command + [obj(source)])
        headers_text = run([OBJDUMP, "-h", *[obj(s) for s in module_sources[name]]])
        sections[name] = [kind for kind in ("data", "bss") if f"ovl_{name}_{kind}" in headers_text]

    for source in game if WINDOWS else []:
        rename_coff_sections(obj(source), {".text": "game_text$m", ".rdata": "game_rodata$m",
                                           ".data": "game_data$m", ".bss": "game_bss$m"})
    for source in game if not WINDOWS else []:
        run([OBJCOPY, "--rename-section=.text=game_text", "--rename-section=.rodata=game_rodata",
             "--rename-section=.data=game_data", "--rename-section=.sdata=game_data",
             "--rename-section=.bss=game_bss", "--rename-section=.sbss=game_bss", obj(source)])
    fixed = dict(FIXED_SECTIONS) if not WINDOWS else {}
    for index, (name, _, _, bank) in enumerate(module for module in MODULES if module[3] and not WINDOWS):
        fixed[f"ovl_{name}_data"] = MODULE_SECTIONS + index * 0x400000
        fixed[f"ovl_{name}_bss"] = MODULE_SECTIONS + index * 0x400000 + 0x200000
    digest = hashlib.sha256()
    for path in sorted(game + [h for h in glob.glob("src/**/*.h", recursive=True) if not h.startswith("src/pc/")]):
        with open(path, "rb") as handle:
            digest.update(path.encode() + b"\0" + handle.read())
    digest.update(" ".join(CFLAGS).encode() + repr(sorted(fixed.items())).encode())

    game_defined, tentative, undefined = symbols([obj(s) for s in game])
    native_defined, _, native_undefined = symbols([obj(s) for s in NATIVE])
    with open("config/slus_01411/functions.csv") as handle:
        rows = list(csv.DictReader(handle))
    functions = {row["name"]: row["status"] for row in rows}
    overlay_rows = []
    for name, _, identifier, bank in MODULES:
        with open(f"config/slus_01411/overlays/{MODULE_CONFIG.get(name, name)}_functions.csv") as handle:
            for row in csv.DictReader(handle):
                row["name"] = renamed.get(name, {}).get(row["name"], row["name"])
                row["bank"], row["identifier"] = bank, identifier if bank else 0
                overlay_rows.append(row)
    by_address = {int(row["address"], 16): row["name"] for row in rows}
    addresses, text = {}, (0, 0)
    for line in run([READELF, "-SW", ELF]).splitlines():
        parts = line.replace("[", " ").replace("]", " ").split()
        if len(parts) > 5 and parts[1] == ".text":
            text = (int(parts[3], 16), int(parts[3], 16) + int(parts[5], 16))
    addresses.update(resident_elf)
    for name, _, _, _ in MODULES:
        for symbol, address in module_elf[name].items():
            addresses.setdefault(renamed.get(name, {}).get(symbol, symbol), address)

    # A native definition replaces the game's: weaken the original so the
    # linker prefers src/pc/overrides (calls are symbol-relative at -O0).
    overridden = sorted(game_defined & native_defined)
    set_overridden = set(overridden)
    if WINDOWS:
        # No weak COFF definitions from objcopy: the native objects come
        # first in the link and lld keeps the first definition. Anything else
        # defined twice is still an error, as on Linux.
        twice = sorted(name for name, count in definitions([obj(s) for s in game + NATIVE]).items()
                       if count > 1 and name not in overridden)
        if twice:
            sys.exit("defined more than once: " + ", ".join(twice[:20]))
    elif overridden:
        # One listing of every game object rather than one `nm` for each:
        # spawning 546 of them cost five seconds of every build, which is
        # most of what `./play.sh` spends before the game appears. -A puts
        # the file each symbol came from at the head of its line.
        weaken, objects = {}, {obj(s) for s in game}
        for line in run([NM, "-A", "-g", "--defined-only", *sorted(objects)]).splitlines():
            path, _, rest = line.partition(":")
            if path not in objects:
                sys.exit(f"{NM} -A named an object the build does not know: {line}")
            if rest.split() and c_name(rest.split()[-1]) in set_overridden:
                weaken.setdefault(path, []).append(c_name(rest.split()[-1]))
        for source in game:
            hits = weaken.get(obj(source))
            if hits:
                run([OBJCOPY, *[f"--weaken-symbol={name}" for name in hits], obj(source)])
    wanted = (undefined | tentative) - game_defined - native_defined - HOST_LIBC
    pinned, stubs, unknown, aliases = {}, [], [], {}
    for name in sorted(wanted):
        address = addresses.get(name)
        # Sources still using a function's address-based name reach the
        # renamed C definition, as the PS1 link's symbol aliases do.
        current = by_address.get(address if address is not None else
                                 int(name[5:], 16) if name.startswith("func_8") and len(name) == 13 else -1)
        if current and current != name and current in game_defined | native_defined:
            aliases[name] = current
            continue
        if name in functions or (address is not None and text[0] <= address < text[1]):
            stubs.append(name)
        elif address is not None:
            pinned[name] = address
        else:
            unknown.append(name)
    # SDK globals that native library ports share with game code.
    for name in native_undefined - game_defined - native_defined:
        if name.startswith("D_8") and name in addresses:
            pinned[name] = addresses[name]
    # Overlay entry points and data live outside the resident image.
    stubs += [name for name in unknown if name in undefined]
    if WINDOWS:
        # lld reads no GNU linker scripts: pins are absolute symbols from an
        # assembly file, and aliases rename the references in the objects
        # (lld does not resolve a symbol defined as another undefined one).
        with open(f"{options.build}/guest_symbols.s", "w") as handle:
            handle.writelines(f".globl _{name}\n.set _{name}, 0x{address:08X}\n" for name, address in pinned.items())
        for source in game:
            unset_coff_commons(obj(source), set(pinned))
        if aliases:
            with open(f"{options.build}/aliases.txt", "w") as handle:
                handle.writelines(f"_{name} _{target}\n" for name, target in sorted(aliases.items()))
            for source in game:
                if set(run([NM, "-u", obj(source)]).split()) & {"_" + name for name in aliases}:
                    run([OBJCOPY, f"--redefine-syms={options.build}/aliases.txt", obj(source)])
        # __start_/__stop_ for the sections state.c and the module registry
        # walk: marker sections that sort before and after the contents.
        with open(f"{options.build}/section_markers.s", "w") as handle:
            marked = [("game_text", "xr"), ("game_data", "dw"), ("game_bss", "bw")]
            marked += [(f"ovl_{name}_{kind}", "dw" if kind == "data" else "bw")
                       for name, _, _, bank in MODULES if bank for kind in sections[name]]
            for section, flags in marked:
                handle.write(f'.section {section}$a,"{flags}"\n.globl ___start_{section}\n___start_{section}:\n')
                handle.write(f'.section {section}$z,"{flags}"\n.globl ___stop_{section}\n___stop_{section}:\n')
    else:
        with open(f"{options.build}/guest_symbols.ld", "w") as handle:
            handle.writelines(f"{name} = 0x{address:08X};\n" for name, address in pinned.items())
            handle.writelines(f"{name} = {target};\n" for name, target in aliases.items())
    with open(f"{options.build}/stubs.c", "w") as handle:
        handle.write('#include "pc/guest/image.h"\n')
        handle.write(f"const unsigned Memories_GameFingerprint = 0x{digest.hexdigest()[:8]}u;\n")
        handle.writelines(f'void {name}(void) {{ Memories_Unimplemented("{name}"); }}\n'
                          for name in sorted(stubs))
        # Guest address -> native function, for calls through pointers stored
        # in the retail data image (see on_fault in src/pc/guest/image.c).
        linked = game_defined | native_defined | set(stubs) | set(aliases)
        mapped = [(int(row["address"], 16), row["name"], row.get("bank", 0), row.get("identifier", 0))
                  for row in rows + overlay_rows if row["name"] in linked]
        # MODEL.MRG swaps per-monster MIPS control modules into these four
        # fixed entry addresses. Native bridges preserve their lifecycle
        # contract until the individual choreography modules are translated.
        mapped += [(0x8013A004, "Memories_ModelPrimaryControlA", 0, 0),
                   (0x8013B004, "Memories_ModelVariantControlA", 0, 0),
                   (0x801462B0, "Memories_DuelEffectControl", 0, 0),
                   (0x8017A004, "Memories_ModelPrimaryControlB", 0, 0),
                   (0x8017B004, "Memories_ModelVariantControlB", 0, 0)]
        mapped.sort()
        handle.writelines(f"extern void {name}(void);\n" for name in sorted({m[1] for m in mapped} - set(stubs)))
        handle.write("const MemoriesGuestFunction Memories_FunctionMap[] = {\n")
        handle.writelines(f"    {{0x{address:08X}u, {name}, 0x{bank:08X}u, 0x{identifier:X}u}},\n"
                          for address, name, bank, identifier in mapped)
        handle.write(f"}};\nconst unsigned Memories_FunctionMapCount = {len(mapped)};\n")
        shared = [(name, identifier, bank) for name, _, identifier, bank in MODULES if bank]
        for name, _, _ in shared:
            for kind in sections[name]:
                handle.write(f"extern char __start_ovl_{name}_{kind}[], __stop_ovl_{name}_{kind}[];\n")
        handle.write("const MemoriesModule Memories_Modules[] = {\n")
        for name, identifier, bank in shared:
            ranges = [f"__start_ovl_{name}_{kind}, __stop_ovl_{name}_{kind}" if kind in sections[name] else "0, 0"
                      for kind in ("data", "bss")]
            handle.write(f'    {{"{name}", 0x{bank:08X}u, 0x{identifier:X}u, {", ".join(ranges)}}},\n')
        handle.write(f"}};\nconst unsigned Memories_ModuleCount = {len(shared)};\n")
    run([CC, *NATIVE_CFLAGS, "-c", f"{options.build}/stubs.c", "-o", f"{options.build}/stubs.o"])
    output = f"{options.build}/memories-pc"
    if WINDOWS:
        output += ".exe"
        for name in ("guest_symbols", "section_markers"):
            run([CC, "-c", f"{options.build}/{name}.s", "-o", f"{options.build}/{name}.o"])
        # The pins first: a game unit's tentative definition of a pinned
        # variable is a COMMON symbol, which the linker script's assignment
        # overrides on Linux; lld keeps whichever it saw first. Then the
        # native objects, which win over the game definitions they override.
        # Large-address-aware for guest RAM at 0x80000000, fixed base (like
        # -no-pie) for the symbol table, NX for the guest-call trap.
        # --export-all-symbols and the import library are what -rdynamic is
        # on Linux: a mod's DLL (build_mods) links against the import library
        # and binds to the game's and the port's symbols at load time.
        run([CC, "-o", output, "-Wl,--large-address-aware", "-Wl,--disable-dynamicbase", "-Wl,--nxcompat",
             "-Wl,--allow-multiple-definition", "-Wl,--export-all-symbols",
             f"-Wl,--out-implib={options.build}/{MOD_IMPLIB}", f"{options.build}/guest_symbols.o",
             *[obj(s) for s in NATIVE + game], f"{options.build}/stubs.o", f"{options.build}/section_markers.o",
             f"{WIN32_DEPS}/sdl/lib/libSDL3.dll.a", "-lopengl32", f"{WIN32_DEPS}/lib/libfreetype.a",
             f"{WIN32_DEPS}/lib/libpng16.a", f"{WIN32_DEPS}/lib/libzs.a", "-ldbghelp", "-static", "-lpthread"])
        shutil.copy(f"{WIN32_DEPS}/sdl/bin/SDL3.dll", options.build)
    else:
        # -rdynamic puts the executable's symbols in its dynamic table, which is
        # what lets a mod's library (build_mods) bind to the game and the port
        # the way the resident code does. Nothing else needs it.
        run(["gcc", "-m32", "-no-pie", "-rdynamic", "-o", output,
             *[f"-Wl,--section-start={name}=0x{address:08X}" for name, address in sorted(fixed.items())],
             *[obj(s) for s in game + NATIVE],
             f"{options.build}/stubs.o", f"{options.build}/guest_symbols.ld", *(["-lm", f"{SDL_BUILD}/libSDL3.a", "-lGL", "-ldl", "-lpthread", "-lfreetype", "-lfontconfig", "-lpng16"] if options.backend == "sdl"
               else ["-lm", "-lX11", "-lXext", "-lfreetype", "-lfontconfig", "-lpng16", "-lasound", "-lpthread", "-ldl"])])
    build_mods(options.build)
    # Save states are carried between builds with these tables
    # (src/pc/guest/state.c): every function in the executable, because the
    # game keeps pointers to native routines as well as its own (HMD
    # primitive drivers, callbacks), and the game objects' variables.
    # "address size name"; repeats of a static name get #2, #3... in address
    # order, which follows the link order. The table's hash is the build id.
    os.makedirs(f"{options.build}/symbols", exist_ok=True)
    seen, table = {}, []
    for line in run([NM, "-n", "-S", output]).splitlines():
        parts = line.split()
        if len(parts) != 4 or not c_name(parts[3]):
            continue
        parts[3] = c_name(parts[3])
        address = int(parts[0], 16)
        in_game = any(start <= address < start + 0x00400000 for start in fixed.values())
        if parts[2] in "Tt" or (parts[2] in "DdBb" and in_game):
            seen[parts[3]] = seen.get(parts[3], 0) + 1
            name = parts[3] if seen[parts[3]] == 1 else f"{parts[3]}#{seen[parts[3]]}"
            table.append(f"{parts[0]} {parts[1]} {name}\n")
    if WINDOWS:
        # PE symbols carry no sizes: a function runs to the next symbol, so
        # crash and hang reports can name the routine an address is in.
        rows = [row.split() for row in table]
        for index, row in enumerate(rows):
            if int(row[1], 16) == 0 and index + 1 < len(rows):
                row[1] = f"{int(rows[index + 1][0], 16) - int(row[0], 16):08x}"
        table = [" ".join(row) + "\n" for row in rows]
    build_id = hashlib.sha256("".join(table).encode()).hexdigest()[:8]
    for name in (build_id, digest.hexdigest()[:8]):  # the second serves states saved before build ids
        with open(f"{options.build}/symbols/{name}.txt", "w") as handle:
            handle.writelines(table)
    with open(f"{options.build}/buildid", "w") as handle:
        handle.write(build_id + "\n")
    kinds = {name: functions.get(name, "outside_resident_image") for name in stubs}
    report = {"game_units": len(game), "pinned_data_symbols": len(pinned),
              "stubbed": {kind: sorted(n for n in stubs if kinds[n] == kind)
                          for kind in sorted(set(kinds.values()))}}
    with open(f"{options.build}/link-report.json", "w") as handle:
        json.dump(report, handle, indent=1)
    print(f"{output}: {len(game)} game units, {len(pinned)} pinned data symbols, " +
          ", ".join(f"{len(v)} {k} stubs" for k, v in report["stubbed"].items()))

if __name__ == "__main__":
    main()
