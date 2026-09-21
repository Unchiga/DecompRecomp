# Native PC port plan

Planning baseline: 2026-09-20, memories-decomp commit
`bef80f7945c86e4c99da8fbd9cc52986d1a86182`.
Status: initial native foundation and pinned PSY-Z probe implemented. See
[build instructions and current limits](pc-build.md). The original planning
milestones below remain the roadmap; the game itself does not boot natively yet.
Retail packet collection and raw GP0 submission now pass synthetic offscreen
textured-render checks. [Frontend contract findings](pc-frontend-contracts.md)
identify the next LIBGS, LIBDS and scheduling work.

**Memory-model decision (2026-09-20, user):** bring the game up as a 32-bit
(ILP32) executable with guest RAM mapped at its real addresses; see
[pc-build.md](pc-build.md#32-bit-game-executable-bring-up). This supersedes the
"avoid fixed host addresses" and x86-64-first defaults below for bring-up;
64-bit is revisited after the menu/duel milestones. Status: the native build
boots from the disc image through the logo screens to the title, main menu and
Options, with the intro FMV, XA/sequenced music and sound effects
(milestone 2's menu is reachable). Next, in
order: (1) a loaded-module registry for the four overlays sharing `0x80168000`
(NEW GAME stops at the name-entry module), including per-load reset of their
C-defined data; (2) memory-card files; (3) the remaining 80 SDK stubs as
reached, LIBGTE math against Psy-Q tables; (4) SPU reverb and Gaussian
interpolation, audio checked against an emulator; (5) the 61 assembly renderers for 3D duels; (6) Windows
mapping/window/audio backends and an emulator-based frame comparison.

## Objective and initial scope

Build a native Windows/Linux x86-64 version of the North American
SLUS-01411 game, using its existing C game logic and user-supplied disc assets.
Start development on Linux and keep Windows builds in CI from the first host
build. Preserve original game rules, RNG, frame cadence, rendering order, audio,
and save behavior. Higher resolution, widescreen, unlocked frame rates, and
gameplay changes come after compatibility. These platform/scope choices are
planning defaults, not requirements established by the user.

Use a separate native target and port-specific adapters, retaining the existing
PS1 matching build as a reference. Do not require host machine code to match
MIPS bytes. Shared-source changes must preserve the reference build; native
replacements are selected by the build, not substituted into PS1 manifests.

## What the audit establishes

The authoritative resident `config/slus_01411/functions.csv` contains:

| Classification | Functions | Port implication |
|---|---:|---|
| Matching game C | 1,134 | Reusable logic, still needs a host portability audit |
| Handwritten game assembly | 61 / 40,116 bytes | Implement portable equivalents |
| Psy-Q CRT/SDK assembly | 591 | Replace the required API boundary and runtime semantics |

The 61 are standalone handwritten assembly targets, not simply 61 inline-asm
blocks in otherwise portable C. The C code also uses GTE macros, fixed scratchpad
addresses, symbol aliases, linker-defined storage, and binary-layout assumptions.
For example, `display_object_core.c` builds primitives at `0x1F800320` and
`0x1F800344`; `func_80034830.c` invokes GTE operations from matched C.

All 97 inventoried functions in the five configured overlays match C. This does
**not** establish complete coverage of executable code on the disc. The runtime
loader notes identify executable payloads in WA, MODEL, and SU banks beyond those
five modules. Inventory those paths before estimating completion. Historical
research notes contain stale counts/classifications; use current manifests for
status and treat older notes as evidence to recheck.

The initial planning pass had no game inputs. The subsequent implementation
pass imported the user's disc, verified the exact BIN hash and extracted all
eight tracked executable/DATA inputs with matching hashes. The canonical local
CUE also matches; the user's source files were left unchanged. Reference-build
results are recorded in `pc-build.md` separately from native validation.

## Architecture decisions

Use CMake for the native target, compile game C as C, and isolate C++ dependency
interfaces behind C-compatible wrappers. Proposed layout:

```text
src/pc/                 host entry, scheduling, input, files, saves
src/pc/compat/          Psy-Q-facing adapters and fixed-width ABI contracts
src/pc/render/          packet translation and GTE boundary
src/pc/asm_replacements/ portable implementations of the 61 routines
src/pc/overlays/        module registry and lifecycle adapters
tests/pc/              behavioral fixtures, replay and integration checks
```

Existing relative `../psyq/...` includes mean include-directory ordering alone
cannot select a new SDK. Introduce an explicit native header-selection boundary
while retaining the reference declarations for the PS1 build. Resolve differing
caller views in adapter functions; do not blindly replace every prototype with
a nominally compatible library declaration.

### Memory and 64-bit ABI

Keep serialized structures, GPU packet words, and asset addresses explicitly
32-bit. Host-owned objects and host function pointers can use native pointers.
Use checked address/offset translation for guest-layout data; never truncate
host pointers into PS1 words. Establish one owner for each storage region and
make named aliases refer to that same storage, avoiding disconnected copies of
globals that were adjacent or overlapping in the original image.

The first architecture experiment must prove mixed pointer-bearing structures,
scratchpad access, data relocations, and one complete ordering-table chain on
x86-64. Audit `sizeof`, `offsetof`, `long`, bitfields, packed/unaligned access,
endianness, integer/pointer casts, aliasing, and signed overflow/shifts. Use
explicit helpers where PS1 arithmetic behavior is part of the contract.
`uintptr_t` fixes pointer transport, not a serialized four-byte field.

Replace scratchpad literals with a shared, aligned, bounded 1 KiB storage API;
preserve intentionally overlapping temporary ranges. Translate hardware register
accesses to subsystem operations. Avoid making fixed host virtual addresses a
requirement. A 32-bit executable can aid diagnosis but is not the final memory
model and does not solve 24-bit GPU links or absolute addresses.

Keep original packet layouts initially and translate them at a rendering boundary.
PsyCross's host packet tags can differ from retail tags: its `libgpu.h` uses
`uintptr_t` in host configurations. Prove packet conversion and ordering rather
than overlaying those structs on original data or bulk-changing their sizes.

### Overlays and assets

Compile recovered overlay C into the host executable with module-qualified
symbols. Replace loading/executing MIPS bytes with a registry keyed by module
identity and entrypoint, preserving load-time data initialization, BSS resets,
callback lifetime, and shared-bank semantics. A virtual address alone is not an
identity because modules reuse addresses. Calls to interior labels and the
overworld alternate family need explicit investigation.

Inventory every executable package in `WA_MRG.MRG`, `MODEL.MRG`, and `SU.MRG` and
trace its entrypoints. Any reachable code without portable source is additional
port scope. Keep it visible as a blocker rather than counting it among the 61.
The first boot/demo may cover a subset; a full release must cover all reachable
gameplay paths. A permanent MIPS interpreter was outside the native-port default until
2026-09-21, when the WA effect bank and the 1,181 MODEL control modules were
put under one (see [pc-build.md](pc-build.md#mips-only-effects)) rather than
translated one by one; that decision stands unless a translated form exists.

Reuse the tracked image hashes, disc layout, and extraction tooling. Keep raw
BIN/CUE sector access available for XA/STR, since ordinary extracted-file reads
can lose sector metadata. Separate executable payloads from asset data without
discarding tables embedded beside code. Initially preserve LBA-facing loader
contracts through an adapter and maintain asynchronous callback order.

## Psy-Q replacement choice

**Evaluate pinned PSY-Z first, behind our own adapters.** The initial PsyCross
recommendation is superseded by the [PSY-Z comparison](pc-sdk-evaluation.md): its
current tree includes partial LIBGS and software SPU/XA paths relevant to this
game. Keep PsyCross as an alternative; neither is validated for the complete game.

The original PsyCross audit follows for comparison. It provides
host graphics, software GTE, controller and CD interfaces, and OpenAL-based SPU
support. It is a practical foundation, not a complete replacement for this game.
Inspected revision: `e56e4cde1c2b8a15e0d4e38b26cdd9202e0d17e6`.

Primary sources:
- [PsyCross source and capability notes](https://github.com/OpenDriver2/PsyCross)
- [PsyCross GPU packet layouts](https://github.com/OpenDriver2/PsyCross/blob/e56e4cde1c2b8a15e0d4e38b26cdd9202e0d17e6/include/psx/libgpu.h)
- [PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK): a PS1 SDK, not the proposed desktop runtime.

The inspected PsyCross tree has no LIBGS or LIBDS implementation. It declares
LIBPRESS APIs without implementing the needed decoder, and its README lists
MDEC, XA/CD audio, and ADSR work as TODOs. This game calls `Gs*`, `DsPacket`,
`DsStartReadySystem`, and `DecDCT*`, and loads `MOVIE.STR` and `MASTER.XA`.
Therefore those are concrete gaps, not optional future enhancements.

| Area | Proposed implementation | Acceptance requirement |
|---|---|---|
| GPU/VRAM | PsyCross plus original-packet adapter | CLUTs, texture pages/windows, draw environments, transparency, ordering |
| GTE and inline macros | Validated software GTE shared by C and ASM replacements | Fixed-point rounding, saturation, FIFOs and FLAG behavior |
| LIBGS | Game-used subset over GPU/GTE adapters | Coordinates, object sorting, buffers and callbacks used by actual callers |
| LIBCD/LIBDS | Disc/file backend plus explicit LIBDS state machine | Sector order, completion, errors, cancellation and callback timing |
| SPU and sequencer | Preserve game sound driver; evaluate PsyCross SPU | Voice behavior, pitch, ADSR, reverb and transfer completion |
| XA/STR/MDEC | Separately selected decoder/backend where PsyCross lacks coverage | Sound/video sync, skip/end behavior and clean return to game |
| Input and timing | Host events mapped to original pad state and frame services | Held/repeat behavior, disconnect, focus and two-player input |
| Memory card | File-backed original-format card/save operations | Read/write round trip, checksums, errors and atomic persistence |
| CRT/BIOS | Host adapters and minimal required compatibility | Startup, callbacks, allocation, setjmp/longjmp and exact game RNG |

Generate an API coverage matrix from actual calls and address/function tables:
symbol, caller, ABI, backend implementation, stub status, semantics test, priority.
591 preserved SDK routines are not 591 independent APIs that must be recreated;
many are implementation internals. Conversely, an exported symbol or header does
not establish usable coverage. Make unsupported runtime calls fail visibly in
development rather than silently succeed.

Select any alternate SPU/video backend only after a small fixture demonstrates
the gap and the replacement; record revision, integration cost, and license
obligations. Review distribution rights for project and dependency code before
packaging. Do not ship original SDK objects, disc assets, or generated game blobs.

## Replacing the 61 routines

Current metadata and the GTE classification research give these structural
cohorts (ends exclusive). These are implementation groupings, not proven semantic
equivalence classes:

| Address span | Count | Initial work unit |
|---|---:|---|
| `0x800612C0–0x80067220` | 32 | Large GTE/primitive variants; establish common math/packet helpers |
| `0x80067220–0x80067354` | 1 | Leaf routine with separate contract |
| `0x80067354–0x80069E44` | 16 | Second object-layout cohort |
| `0x80069E44–0x8006A99C` | 8 | Smaller cohort; candidate for first representative |
| `0x8006A99C–0x8006AF74` | 4 | Final small cohort |

The accompanying `pc-port-asm-inventory.csv` tracks all 61 originals, sizes,
cohorts and replacement status. Start with `func_80069E44` after confirming its
call contract, then one member of each cohort and the leaf before expanding.

For each function:
1. Recover arguments, input/output layouts, globals, GTE entry/exit state,
   packet links, and every caller, including indirect dispatch.
2. Capture reference input/output fixtures by running original MIPS in a
   PS1-capable emulator harness with GTE and required memory initialized. Record
   modified memory, return values, packet streams, and relevant GTE state.
3. Write literal portable fixed-width C first. Preserve delay-slot effects and
   any observable pipeline behavior; do not reproduce register scheduling for
   its own sake. Hardware latency can be abstracted only where unobservable.
4. Compare boundary cases: empty/minimum/max counts, triangles/quads, clipping,
   negative coordinates, depth limits, overflow, lighting saturation, texture
   flags and ordering-table insertion. Add captured real-game fixtures.
5. After equivalence, consolidate proven shared logic with thin per-entrypoint
   wrappers. Do not assume all variants differ only in primitive type.

Sixty routines save registers into caller-object slots. Determine whether those
scratch writes are observable before excluding them from comparisons. Keep any
required memory side effects; document exclusions for dead save slots rather
than claiming literal equality to host register values.

## Delivery milestones and gates

| Milestone | Deliverables | Exit gate |
|---|---|---|
| 0. Reference and coverage | Pinned source, verified disc inputs, PS1/overlay baselines, runtime-package inventory, SDK matrix, memory audit | Known executable paths mapped; unknown packages explicitly budgeted; reference traces reproducible |
| 1. Host feasibility | CMake Linux/Windows builds, header boundary, guest layouts, GTE tests, translated packet chain, callback scheduler | Correct textured frame with input on x86-64; no pointer truncation; representative GTE cases match |
| 2. Frontend slice | Asset loading, LIBGS/LIBDS subset, native main-menu overlay, font/UI drawing, original pad repeat | Normal startup reaches a usable menu; repeatable menu navigation matches reference |
| 3. Duel slice | Deck selection, AI/game state, required overlays, renderer replacements reached by slice | Complete one free duel through reward and return; fixed-input state/RNG replay agrees |
| 4. Full gameplay | All 61 replacements, remaining discovered code, campaign, passwords, deck/library, two-player, animated battles, saves | Every required mode covered; campaign progression and save/reload checkpoints pass; no reachable MIPS fallback |
| 5. Media and release | Complete sound, XA, movies, error handling, packaging and performance | Media stays synchronized; retail-like presentation; fresh Windows/Linux installs import assets and play successfully |

Audio/video experiments begin in milestone 1 alongside the renderer feasibility
work, because backend selection can change the schedule. A documented movie skip
or silent development build can unblock early slices but cannot pass release.
Work on the 61 continues through milestones 1–4 in dependency order; replacing
all of them before reaching the first menu would delay useful integration feedback.

Do not assign a reliable calendar estimate until milestones 0–1 establish extra
overlay scope and backend gaps. Estimate by completed/tested cohort and API work,
not by raw function count.

## Validation strategy

Maintain three independent checks:
- PS1 byte matching (`make match`, `make match-overlays`) for shared-source
  regressions when the required reference toolchain and inputs are available.
- Host unit/differential tests for integer math, GTE, packet translation, loader
  transitions, data layouts, all 61 replacements, and save serialization.
- End-to-end replay with fixed per-frame inputs, initial state, and RNG seeds.
  Compare game state/events separately from rendered images and audio timing.

The RNG must implement the documented 32-bit wraparound recurrence
`state = state * 1103515245 + 12345`, returning `(state >> 16) & 0x7fff`.
Do not delegate to host libc `rand()`. Preserve the original seeding/call order.
Drive simulation and VBlank services with a fixed original cadence independent
of monitor refresh, and deliver asynchronous completions at defined points.

Use exact equality for integer gameplay state and decoded packet fields, and
document tolerances for backend image/audio comparisons. Normalize relocated
addresses before comparing snapshots. Exercise sanitizers on host builds and
pointer/layout diagnostics on both platforms. Headless tests should not require
proprietary assets where synthetic fixtures suffice; reference asset integration
tests run locally with user-supplied inputs.

Release coverage includes a complete campaign, new/load/save, free duel rewards,
password purchases and invalid input, deck edits/library, two-player pads,
animated battles, intro/ending movies, audio transitions, and repeated overlay
switching. Test missing assets, corrupt saves and interrupted writes as behavior,
not merely happy-path boot.

## First implementation batch

1. Establish the verified reference inputs and reproduce the two matching gates.
2. Produce executable-package, SDK-call, global-storage and layout inventories.
3. Add the host build and explicit header/ABI boundary; resolve one packet chain
   and one pointer-bearing object on x86-64.
4. Run the pinned PSY-Z probe, then a rendered GPU/GTE demo and LIBDS/audio/video gap probes.
5. Differentially replace `func_80069E44`, then one representative per cohort.
6. Use those results to lock the backend and memory design, update estimates,
   and connect the real startup/main-menu path.

Relevant local evidence: `config/slus_01411/functions.csv`,
`config/slus_01411/overlays.json`, `notes/function-map.md`,
`notes/gte-classification-audit.csv`, `notes/psyq.md`, `notes/rng.md`,
`notes/overlays/runtime-loader.md`, and `src/overlays/overworld/README.md`.
