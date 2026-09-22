#!/usr/bin/env python3
"""Compile and link the resident game C as a 32-bit Linux executable.

Bring-up driver for the fixed-address memory model (src/pc/guest/image.h):
  * every game unit is compiled with the host GCC as ILP32;
  * data symbols the units leave undefined or tentative are pinned to their
    retail addresses, read from the matching build's ELF;
  * undefined functions get generated stubs that name themselves and exit,
    unless a native source under src/pc already defines them.
Requires the matching build's ELF (make match) for symbol addresses."""
import argparse, concurrent.futures, csv, glob, hashlib, json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ELF = "tmp/project-build/SLUS_014.11.elf"
CFLAGS = ["-m32", "-std=gnu11", "-fpermissive", "-w", "-O0", "-g", "-fno-strict-aliasing",
          "-fwrapv", "-fcommon", "-fno-pie", "-fno-stack-protector", "-DMEMORIES_PC",
          "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Isrc"]
# -O0 for game units: original busy-waits poll non-volatile globals that the
# VBlank handler updates, and must not be hoisted out of their loops.
NATIVE_CFLAGS = ["-m32", "-std=gnu11", "-O2", "-g", "-Wall", "-fno-pie", "-fno-omit-frame-pointer", "-fno-strict-aliasing",
                 "-Wno-builtin-declaration-mismatch", "-DMEMORIES_PC", "-D_LANGUAGE_C", "-DLANGUAGE_C", "-Isrc",
                 "-I/usr/include/freetype2"]
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

def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        sys.exit(f"{' '.join(command[:6])} ...\n{result.stderr}")
    return result.stdout

def compile_unit(job):
    source, obj, flags, renames = job
    if os.path.exists(obj) and os.path.getmtime(obj) >= NEWEST_HEADER and \
            os.path.getmtime(obj) >= os.path.getmtime(source):
        return
    run(["gcc", *flags, "-c", source, "-o", obj])
    if renames:
        run(["objcopy", f"--redefine-syms={renames}", obj])

def symbols(objects):
    defined, tentative, undefined = set(), set(), set()
    for line in run(["nm", "-g", *objects]).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] in "UCTDBRVW" and not line.endswith(":"):
            {"U": undefined, "C": tentative}.get(parts[-2], defined).add(parts[-1])
    return defined, tentative, undefined

def main():
    global NEWEST_HEADER
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=list(BACKENDS), default=os.environ.get("MEMORIES_BACKEND") or
                        ("sdl" if os.path.exists(f"{SDL_BUILD}/libSDL3.a") else "x11"))
    parser.add_argument("--build", default="tmp/pc/game32")
    options = parser.parse_args()
    NATIVE.extend(BACKENDS[options.backend])
    NATIVE.sort()
    if options.backend == "sdl":
        if not os.path.exists(f"{SDL_BUILD}/libSDL3.a"):
            sys.exit(f"{SDL_BUILD}/libSDL3.a is missing; build SDL3 for -m32 first (notes/pc-build.md)")
        NATIVE_CFLAGS.extend([f"-I{SDL_SOURCE}/include", f"-I{SDL_BUILD}/include-revision"])
    os.chdir(ROOT)
    if not os.path.exists(ELF):
        sys.exit(f"{ELF} is missing; run `make match` first")
    os.makedirs(options.build + "/obj", exist_ok=True)
    headers = glob.glob("src/**/*.h", recursive=True) + [__file__, "config/pc/host_symbol_renames.txt"]
    NEWEST_HEADER = max(os.path.getmtime(path) for path in headers)
    obj = lambda source: f"{options.build}/obj/{source.replace('/', '_')}.o"
    # main_menu is the only overlay with a private load address (0x80180000),
    # so it can simply be linked in. The 0x80168000 modules share one address
    # and need a loaded-module registry first.
    resident = sorted(glob.glob("src/game/*.c"))
    module_sources = {name: sorted(glob.glob(pattern)) for name, pattern, _, _ in MODULES}
    game = resident + [source for name, _, _, _ in MODULES for source in module_sources[name]]
    jobs = [(s, obj(s), CFLAGS, "config/pc/host_symbol_renames.txt") for s in game]
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
        for line in run(["readelf", "-sW", elf]).splitlines():
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
            command = ["objcopy"]
            for old, new in sorted(renamed[name].items()):
                command.append(f"--redefine-sym={old}={new}")
            for section in (".data", ".sdata"):
                command.append(f"--rename-section={section}=ovl_{name}_data")
            for section in (".bss", ".sbss"):
                command.append(f"--rename-section={section}=ovl_{name}_bss")
            run(command + [obj(source)])
        headers_text = run(["objdump", "-h", *[obj(s) for s in module_sources[name]]])
        sections[name] = [kind for kind in ("data", "bss") if f"ovl_{name}_{kind}" in headers_text]

    for source in game:
        run(["objcopy", "--rename-section=.text=game_text", "--rename-section=.rodata=game_rodata",
             "--rename-section=.data=game_data", "--rename-section=.sdata=game_data",
             "--rename-section=.bss=game_bss", "--rename-section=.sbss=game_bss", obj(source)])
    fixed = dict(FIXED_SECTIONS)
    for index, (name, _, _, bank) in enumerate(module for module in MODULES if module[3]):
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
    for line in run(["readelf", "-SW", ELF]).splitlines():
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
    if overridden:
        for source in game:
            names = set(run(["nm", "-g", "--defined-only", obj(source)]).split())
            hits = [name for name in overridden if name in names]
            if hits:
                run(["objcopy", *[f"--weaken-symbol={name}" for name in hits], obj(source)])
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
    run(["gcc", *NATIVE_CFLAGS, "-c", f"{options.build}/stubs.c", "-o", f"{options.build}/stubs.o"])
    output = f"{options.build}/memories-pc"
    run(["gcc", "-m32", "-no-pie", "-o", output,
         *[f"-Wl,--section-start={name}=0x{address:08X}" for name, address in sorted(fixed.items())],
         *[obj(s) for s in game + NATIVE],
         f"{options.build}/stubs.o", f"{options.build}/guest_symbols.ld", *(["-lm", f"{SDL_BUILD}/libSDL3.a", "-lGL", "-ldl", "-lpthread", "-lfreetype", "-lfontconfig", "-lpng16"] if options.backend == "sdl"
           else ["-lm", "-lX11", "-lXext", "-lfreetype", "-lfontconfig", "-lpng16", "-lasound", "-lpthread"])])
    # Save states are carried between builds with these tables
    # (src/pc/guest/state.c): every function in the executable, because the
    # game keeps pointers to native routines as well as its own (HMD
    # primitive drivers, callbacks), and the game objects' variables.
    # "address size name"; repeats of a static name get #2, #3... in address
    # order, which follows the link order. The table's hash is the build id.
    os.makedirs(f"{options.build}/symbols", exist_ok=True)
    seen, table = {}, []
    for line in run(["nm", "-n", "-S", output]).splitlines():
        parts = line.split()
        if len(parts) != 4:
            continue
        address = int(parts[0], 16)
        in_game = any(start <= address < start + 0x00400000 for start in fixed.values())
        if parts[2] in "Tt" or (parts[2] in "DdBb" and in_game):
            seen[parts[3]] = seen.get(parts[3], 0) + 1
            name = parts[3] if seen[parts[3]] == 1 else f"{parts[3]}#{seen[parts[3]]}"
            table.append(f"{parts[0]} {parts[1]} {name}\n")
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
