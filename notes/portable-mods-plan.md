# Portable mods: one mod for Windows and Linux

Plan written 2026-09-22. Goal: **a mod author makes one mod and it runs on
both the Windows and the Linux build.** Nobody should have to produce a
`.so` and a `.dll`.

Read first: `notes/modding.md` (the current system), `src/pc/mods/modapi.h`,
`src/pc/mods/mods.c`, `tools/pc/build_game32.py` (`build_mods`, the symbol
table), and the "Windows" section of `notes/pc-build.md` (both builds come
from `./build-pc.sh`, and the Windows one is tested under Wine).

## Where things stand

- **Data-only mods** (`mod.json` plus `data` replacements or patches) have
  no code and already work on both systems. Nothing changes for them.
- **Code mods** are native shared libraries: `mods.c` `dlopen`s
  `<library>.so` on Linux and `LoadLibrary`s `<library>.dll` on Windows
  (`pc/compat/dlfcn.h`). Names bind to the game through `-rdynamic` (Linux)
  or the export table plus `libmemories-pc.a` import library (Windows). So
  every code mod needs two builds, and building a `.dll` needs this repo's
  toolchain.
- The two shipped mods are code mods: `mods/3d-monsters/field_models.c`
  (858 lines) and `mods/hand-camera/hand_camera.c` (67 lines).

## The design in one paragraph

Both builds are 32-bit x86 with the same calling convention, so the machine
code is the same; only the container differs. A code mod becomes **one ELF
relocatable object, `<library>.o`**, which the game loads on both systems
with **its own small loader**: map the sections, resolve undefined names
against a table compiled into the executable, apply the relocations, then
call `MemoriesModInit`. The mod is built against a **mod SDK** with no system
C library (glibc and the Windows CRT disagree about `FILE`, `stdin`, `errno`
and more). What a mod needs from the host comes from the mod API or from a
small, fixed list of C library functions the host supplies. One command,
`tools/pc/build_mod.py`, builds a mod on either system.

What was ruled out, and why:
- **Two native libraries**: what we have now, and the problem.
- **Lua or WebAssembly**: one file, but mods could only reach what we
  expose. 3D Monsters needs the model loader, the GPU banks and the ordering
  table directly.
- **TinyCC in the game, compiling mod source at startup**: needs no
  compiler, but puts a compiler in the game, makes mod code slower, and
  shows compile errors on the player's machine. Revisit only if mod authors
  without a compiler turn out to matter.

## Facts established while planning (check them again, they drift)

- Objects built today use `-fPIC`: relocations include `R_386_GOT32X`,
  `GOTOFF`, `GOTPC` and `PLT32`. Built `-fno-pic` they reduce to `R_386_32`,
  `R_386_PC32` and `R_386_PLT32` (treat that one as PC32). The loader should
  only accept those and refuse anything else with a clear message ("build
  with tools/pc/build_mod.py").
- Undefined symbols of the current mods:
  - **hand-camera**: `D_800E9DB0 D_800F2848 Duel_DrawFieldCards
    gDuel_wSceneStateFlags ViewState_ApplyOrbit`
  - **3d-monsters**: game functions (`func_800540B4`, `Duel_DrawFieldCards`,
    `GsClearOt`, `LoadImage`, `StoreImage`, `SetGeomOffset`...), pinned guest
    data (`D_80010000`, `D_800E9D98`, `gDuel_adwCardStats`... absolute
    symbols), port data and functions (`SoftGpu_Bank`,
    `Memories_GpuCommandWords`, `Memories_GteReadControl`...), and libc:
    `clock_gettime getenv malloc memcpy mmap sqrt strtol vsnprintf` (plus
    `__stack_chk_fail_local`, gone with `-fno-stack-protector`).
- `mmap` is used once, for model arenas at fixed guest addresses
  (`ARENA_BASE 0x90000000`, `ARENA_SIZE 0x40000`, `MAP_FIXED_NOREPLACE`).
- The game and psyq headers a mod includes only pull in `<stddef.h>` and
  `<stdint.h>`. Those come with the compiler, so they are safe.
  System-specific includes come from `modapi.h` (`<stdio.h>`, for `FILE *`),
  `pc/compat/mman.h` (`<sys/mman.h>`) and the mods' own `<stdio.h>
  <stdlib.h> <string.h> <math.h> <time.h>`.
- The runtime symbol table (`symbols/<buildid>.txt`, `Symbols_Load`) only
  lists functions and game-section data. It has **no pinned guest symbols**
  and no port data, so it is **not** enough to resolve mods.
- `tmp/pc/llvm-mingw/bin` has `clang` and `ld.lld`. A Windows author can
  build ELF objects with `clang --target=i386-pc-linux-gnu -c`. On Linux,
  `gcc -m32` or the same clang works.

## ABI traps the SDK must close (each needs a test, not a promise)

1. **Stack alignment.** Linux i386 code may assume a 16-byte-aligned stack.
   Windows only guarantees 4. A mod built for Linux that uses SSE on the
   stack crashes on Windows. The SDK builds with `-march=i686 -mno-sse`
   (x87 floating point, as the game's `-m32` default) and
   `-mincoming-stack-boundary=2` (gcc) / `-mstack-alignment=4` (clang).
2. **64-bit and `double` alignment in structures.** i386 Linux aligns
   `long long` and `double` to 4 inside structs. MinGW (Windows) aligns them
   to 8. `src/types.h` has `s64`/`u64`, used in `ygo_types.h` (the
   `u64 all` union near line 206, `Pair64` near line 1247). A mod built with
   the Linux layout could disagree with the Windows game. Add a layout probe:
   a generated C file of `sizeof`/`offsetof` for every struct in the headers
   the SDK ships, compiled by the Windows compiler, the Linux compiler and
   the mod compiler, with a build check that the three agree. Fix any
   mismatch in the header with an explicit `__attribute__((aligned(N)))` so
   both builds match retail (MIPS also aligns 64-bit to 8). This probe also
   protects the Windows game itself, not only mods.
3. **Structs returned by value.** The SysV i386 ABI returns them through a
   hidden pointer that the callee pops. The Windows ABI returns small ones in
   EAX:EDX. A mod calling a Windows-built function that returns a struct
   would break. The build should list game/port functions returning structs
   by value and refuse to export them, or the SDK marks them
   `__attribute__((ms_abi))`-incompatible. Most likely there are none, so
   confirm.
4. **Bitfields.** The Windows build uses `-mno-ms-bitfields`, so it matches
   GCC's layout. The layout probe covers this too (`GsOT_TAG` is the known
   case).
5. **No system C library in a mod.** Build with `-nostdinc` plus the
   compiler's own include directory (for `stddef.h stdint.h stdarg.h
   stdbool.h limits.h float.h`) and the SDK's `include/`, which declares
   only what the host supplies (below).

## Phase 1: the symbol table mods bind to

- `build_game32.py` writes `mod_exports.c` beside `stubs.c`: a sorted table
  `{ "name", (void *)&name }` of every global a mod may use. That is every
  global function and data object defined by the game units and the port's
  native code, plus every pinned guest symbol (`guest_symbols`). The linker
  fills in the addresses, so it is the same mechanism on both systems, with
  no reliance on `-rdynamic` or `--export-all-symbols`.
- Exclude names a mod must not bind to: the host's C library, the mod
  system's internals, and anything with a `static` twin (the symbol-table
  `#2` names).
- `mods.c` gets `static void *lookup(const char *name)`, a binary search.
- Size check: roughly 10k entries. Fine.
- Test: a CTest that the table is sorted, has no duplicates, and resolves a
  known function, a pinned guest symbol and a port data symbol to the same
  addresses the linker gave them.

## Phase 2: the host C library list

- `src/pc/mods/mod_libc.c`: `{ "memcpy", memcpy }, ...` for a fixed
  allowlist. Proposed: `memcpy memmove memset memcmp strlen strcmp strncmp
  strchr strrchr strtol strtoul snprintf vsnprintf malloc calloc realloc
  free abs sqrt sin cos atan2 fabs floor ceil fmod pow qsort`, plus `fread
  fwrite fseek ftell fclose fgets` for the `FILE *` the host hands out
  (`open_asset`/`open_data`). No `fopen`: files only through the mod API.
  `FILE` stays an opaque type in the SDK.
- `lookup` tries the export table first, then this list. An unknown name
  fails the load with the Mods-window status "needs <name>, which this game
  does not provide".
- Bump `MEMORIES_MOD_API` to 2 and add to `MemoriesModHost` what the mods
  used libc for:
  - `uint32_t (*now_us)(const MemoriesModHost *)`: replaces `clock_gettime`.
  - `void *(*map_fixed)(const MemoriesModHost *, uintptr_t address, size_t
    size)`: replaces the `mmap` of 3D Monsters' arenas. `mods.c`
    implements it with `mmap(MAP_FIXED_NOREPLACE)` / `VirtualAlloc`, both
    already behind `pc/compat/mman.h`. Returns NULL if the range is taken.
  - `const char *(*env)(const MemoriesModHost *, const char *name)`: only
    `MEMORIES_*` names, replacing `getenv`. 3D Monsters' tunables are
    debugging knobs. Consider moving them to `host->setting` instead and
    dropping `env`.
  - New entries go at the **end** of the struct, so version-1 fields keep
    their offsets.

## Phase 3: the loader (`src/pc/mods/object_loader.c`)

- Input: an ELF32 `ET_REL` `EM_386` file. Validate everything (header,
  section bounds, symbol and string indexes). This parses untrusted input,
  so it must fail cleanly on bad data rather than crash.
- Allocate each `SHF_ALLOC` section (`.text*`, `.rodata*`, `.data*`,
  `.bss*`) into one block: `mmap` RW on Linux, `VirtualAlloc` RW on Windows.
  Apply relocations. Then make text RX and the rest RW (`mprotect` /
  `VirtualProtect`). Keep it resident for the life of the process, as
  `mods.c` already does for libraries.
- Relocations: `R_386_32` (S + A), `R_386_PC32`/`R_386_PLT32` (S + A - P),
  using the in-place addend (REL, not RELA). Anything else is refused.
- Symbols: defined ones resolve to section base + value. `SHN_COMMON` is
  refused (build with `-fno-common`). Undefined ones go through `lookup`.
  The mod's `MemoriesModInit` is found in its own symbol table.
- Refuse `.init_array`/`.ctors` (no constructors), TLS sections, and
  anything that asks for a GOT.
- Register the mod's function symbols with the crash and hang reporter
  (`src/pc/debug/symbols.c`), so a crash inside a mod reads
  `3d-monsters:draw_frame+0x40` and not a bare address. Optional stretch on
  Linux: the GDB JIT interface (`__jit_debug_register_code`) so gdb sees
  the mod's symbols.
- `mods.c`: `"library": "x"` loads `x.o`. The suffix rule becomes: no
  suffix means `.o`. Drop `LIBRARY_SUFFIX`, `dlopen` and `LoadLibrary` for
  mods. If needed, keep native `.so`/`.dll` loading behind an explicit
  suffix only for development, and delete it once both mods are ported.
- Tests (CTest, both builds): load a small fixture object that calls a
  host function, reads and writes a pinned guest variable, uses
  `.rodata`/`.data`/`.bss`, and calls back through a function pointer. Plus
  negative fixtures: a PIC object, an unknown symbol, a truncated file, a
  COMMON symbol, and a constructor. Each must fail with its own message and
  not crash.

## Phase 4: the SDK and `tools/pc/build_mod.py`

- `build_game32.py` already copies `modapi.h` into `<build>/include`.
  Extend that into an SDK directory shipped beside the game:
  `sdk/include/` holding `modapi.h`, the game and psyq headers mods use (or
  simply `src/` minus `src/pc` internals), and minimal `stdio.h stdlib.h
  string.h math.h` that declare only the phase 2 list, with an opaque
  `FILE`.
- `modapi.h` stops including `<stdio.h>` and uses the SDK's opaque `FILE`.
  Check that the host side still compiles (the host includes the real
  `<stdio.h>` first; guard the typedef).
- `pc/compat/mman.h` must not be reachable from mod code. 3D Monsters
  includes it only indirectly (through `pc/memory.h`?). Find the path and
  break it.
- `tools/pc/build_mod.py <mod dir> [--out DIR]`: finds a compiler (`clang`,
  which on Windows means llvm-mingw's, else `gcc -m32`), compiles every
  `.c` with the fixed flags, and merges them with `ld.lld -r` (or `ld -m
  elf_i386 -r`) into `<library>.o` next to `mod.json`. Flags:
  `--target=i386-pc-linux-gnu` (clang) or `-m32` (gcc), `-std=gnu11 -O2
  -fno-pic -fno-common -fno-stack-protector -fno-asynchronous-unwind-tables
  -march=i686 -mno-sse -mstack-alignment=4 | -mincoming-stack-boundary=2
  -ffreestanding -nostdinc -isystem <compiler include> -I sdk/include
  -mno-ms-bitfields(n/a on ELF, harmless) -DMEMORIES_PC -D_LANGUAGE_C
  -DLANGUAGE_C`. Optionally strip `.debug*` for release, keeping symbols.
- `build_game32.py`'s `build_mods` calls the same code, so the repo's mods
  and an outside author's are built identically. Build each mod **once**
  and copy the `.o` into both `tmp/pc/game32/mods/` and `tmp/pc/win32/mods/`.
  That proves "one file, both systems" on every build.

## Phase 5: port the existing mods

**hand-camera**: nothing to change in the source except the includes.
- Remove `#include <stdlib.h>` (unused) and `<stdint.h>` (fine to keep; it
  comes with the compiler).
- Its binds (`D_800E9DB0`, `D_800F2848`, `gDuel_wSceneStateFlags`,
  `Duel_DrawFieldCards`, `ViewState_ApplyOrbit`) are all in the phase 1
  table.
- `api = MEMORIES_MOD_API` picks up version 2 on rebuild.

**3d-monsters**:
- `clock_gettime` (lines ~368/417, load timing for the log) becomes
  `host->now_us(host)`.
- `mmap(... MAP_FIXED_NOREPLACE ...)` for arenas (line ~447) becomes
  `host->map_fixed(host, ARENA_BASE + i * ARENA_SIZE, ARENA_SIZE)`. Keep
  the "no arena at %p" message.
- `getenv`/`strtol` in `tunable()` (line ~175): move the tunables to
  `host->setting(host, "<key>", fallback)` (they then persist as
  `mod.3d-monsters.<key>` in settings), or use `host->env` if that stays in
  the API. List the tunables and their current `MEMORIES_*` names in the
  commit message so anyone relying on them knows.
- `vsnprintf`, `malloc`, `memcpy`, `sqrt` come from the phase 2 list.
- Includes: drop `<time.h>`. `<stdio.h> <stdlib.h> <string.h> <math.h>`
  now resolve to the SDK's. Break the path to `pc/compat/mman.h`.
- The port data it binds (`SoftGpu_Bank`, `Memories_GpuCommandWords`,
  `Memories_GteReadControl/WriteControl`) must be in the phase 1 table.
  They are port internals, so this mod is tied to the port by design. That
  is fine for a mod the release ships.

**For both**, the acceptance test, on **both** builds (Linux natively,
Windows under Wine, and once on real Windows):
- The mod shows in Game > Mods as loaded, with no status error.
- Hand camera: L1/R1 turn and L3/R3 zoom in a duel with the hand up.
- 3D Monsters: models stand on face-up monsters. The `MEMORIES_TRACE=mods`
  log shows the MODEL.MRG start sector and load times.
- Save state then load with the mods applied: no crash (`reset` hook).
- `python3 tools/pc/smoke.py` and `--windows` still pass (mods off), plus a
  new smoke case with both mods on at a duel frame. It needs an input
  script that reaches a duel. If that is too long, use a save state
  fixture.

## Phase 6: docs and cleanup

- `notes/modding.md`: rewrite "Native mods" as "Code mods". Cover one `.o`
  for every system, `build_mod.py`, the SDK, the libc list, the ABI rules
  (why no system headers), and the trust statement (still native code;
  unchanged).
- Remove the `.dll`/`.so` mod paths from `build_game32.py` (`MOD_IMPLIB`,
  `--export-all-symbols`, `--out-implib`, `-rdynamic`, the DLL link that
  added `guest_symbols.o`), **only once nothing needs them**. `-rdynamic`
  and `--export-all-symbols` may still matter for crash symbolization.
  Check `crash.c`/`symbols.c` before dropping them.
- Keep `pc/compat/dlfcn.h` if anything else uses it; otherwise delete.

## Not in this plan (next, separately)

- **Texture packs from an asset catalog**: data-only, so already portable.
  Someone mapped the WA_MRG/SU art for the static-recomp fork:
  `https://github.com/yamyi/YuGiOhForbiddenMemoriesRecomp/blob/feature/asset-manager/src/psx_wa_catalog.c`.
  It has 45 asset families (card art, thumbnails, fields, UI) and matches
  replacements by hashing VRAM uploads. In this port the match point would
  be `LoadImage` (`src/pc/sdk/libgpu.c`). Start with a probe that counts
  which families match here, then add a `"textures"` key to `mod.json`.
- A CI job building both executables on every push. The retail-file
  secrets already exist for the matching build.

## Rules for whoever implements this

- Keep `./build-pc.sh` building both systems. After every phase, run
  `./build-pc.sh`, `python3 tools/pc/smoke.py`, and
  `python3 tools/pc/smoke.py --windows`.
- Wine is not Windows. Before calling phase 5 done, check the mods once on
  real Windows (`tools/pc/run_debug_windows.bat`).
- Record the ABI facts you verify (layout probe results, any struct-return
  functions found) in this file as you go.
- Do not commit without the user asking.
