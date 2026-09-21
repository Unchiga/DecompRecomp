# Psy-Q replacement evaluation

Inspected 2026-09-20. Primary candidate: [Xeeynamo/psyz](https://github.com/Xeeynamo/psyz),
revision `e2d3a84eb4432c0a80b193eb844edc2bdde90c62`. Alternative:
[OpenDriver2/PsyCross](https://github.com/OpenDriver2/PsyCross), revision
`e56e4cde1c2b8a15e0d4e38b26cdd9202e0d17e6`.

PSY-Z moves ahead of PsyCross for the first integration experiment because its
source contains partial LIBGS, a software SPU with envelope/reverb processing,
and XA decoding. This is evidence of implementation coverage, not proof that
those paths reproduce Forbidden Memories. Its decompiled SDK base also differs
from the game's identified Psy-Q 4.6 contracts.

| Requirement | PSY-Z source evidence | Remaining work |
|---|---|---|
| Native GPU packets | `psyz/include/libgpu.h`: `OT_TYPE`, `O_TAG`, pointer-sized links | Translate retail packet words; native structs cannot overlay raw PS1 packets |
| Software GTE | `psyz/src/psyz/libgte.c`: data/control register APIs and command dispatch | Differential fixtures for all command families the game uses |
| LIBGS | `libgs.c`: graph setup, buffers, clear, draw and OT support | `GsInitVcount`, `GsGetVcount`, clipping/offset helpers are stubs; 3D coordinate/sorting APIs need coverage |
| LIBDS | No implementation found in built sources | Implement the game's queue/completion contracts over the disc backend |
| SPU | `psyz_spu.c`: ADPCM, ADSR, reverb paths | Validate the game sound driver's register/transfer/timing expectations |
| XA | `libcd.c`: XA decoding/resampling | Validate channel selection, formats and game stream transitions |
| STR/MDEC | `libpress.c`: `DecDCT*` entrypoints use `NOT_IMPLEMENTED` | Decoder and callbacks still needed |
| Platform callbacks | `libetc.c`: `ResetCallback` logs `NOT_IMPLEMENTED` | Startup, VBlank and callback scheduling need game-specific review |

PsyCross has GPU/GTE, CD and OpenAL SPU support, but the inspected tree lacks
LIBGS/LIBDS and documents MDEC/XA/ADSR gaps. The decision is to test PSY-Z, not
assume all of its exported APIs are functional.

The optional native probe builds PSY-Z and its pinned SDL submodule. It verifies
positive/negative GTE NCLIP results for a simple triangle and preservation of
host pointers through an ordering-table chain. `ResetGraph(0)` must run before
`ClearOTagR`: it installs host GPU callbacks. Calling OTC first reached an
uninitialized original hardware-register path in the initial probe. The corrected
initialization sequence passes on Linux x86-64.

This contract probe does not open a window, render pixels, decode audio, test all
GTE flags, or establish hardware equivalence. A separate `memories_render_smoke`
now tests retail packet submission through the raw GP0 interface, 16bpp and CLUT
sprite readback, and queue limits. See [build instructions](pc-build.md); those
synthetic rendering checks do not establish game-wide compatibility.

The dependency is optional and pinned in `config/pc/dependencies.json`; the
foundation builds without it. Only the SDL submodule is needed for this native
experiment. Do not run the SDK's root Makefile to bootstrap the PC target.

The [upstream license manifest](https://github.com/Xeeynamo/psyz/blob/e2d3a84eb4432c0a80b193eb844edc2bdde90c62/LICENSE)
lists MPL 2.0 implementation directories, MIT components, and unlicensed headers/
SDK portions. Evaluation builds are local; redistribution review remains an
explicit packaging task. Pinning the SDK does not resolve those permissions.

API inventory command: `cmake --build tmp/pc --target pc_audit`. The generated
JSON lists actual identifier references with file/line locations and marks each
SDK symbol `needs_contract_review`. It is deliberately not an automatic claim
that a symbol is implemented, linked, or semantically compatible.
