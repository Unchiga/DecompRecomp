# Frontend integration contracts

Evidence: current game sources and assembly regenerated from the hash-verified
SLUS-01411 reference. This is the next integration backlog, not a claim that
the frontend is already linked into the native executable.

## Graphics boundary now implemented

`src/pc/render/packets.c` reads little-endian retail DMA tags from guest RAM,
follows 24-bit links, preserves payload order, and snapshots GP0 words. It handles
empty OT entries, commands split across DMA packets, and payloads on the final
node. A caller-supplied hop limit bounds cyclic/oversized lists. Invalid pointers,
unaligned tags, spans crossing RAM, and insufficient output capacity fail with
explicit status codes. No host pointer is stored in a retail packet.

`src/pc/render/psyz_gpu.c` validates the complete stream before submitting any
words. It uses `Psyz_GpuWriteGP0` and flushes at complete command boundaries below
the pinned SDK's 16,384-word queue limit. This bypasses the need to widen game
primitive structs to match the SDK's native packet layouts. The supported subset
is NOPs, fixed-size polygon/line/rectangle primitives, fill, VRAM copy and E1–E6
drawing state. Polylines, raw CPU/VRAM transfer commands and other unsupported
opcodes fail explicitly. Texture uploads/readback currently use SDK APIs outside
the DMA stream. Submission is single-threaded and synchronous.

The render smoke test verifies 16bpp and 4bpp/CLUT textures, exact RGB555 readback
over a 320x240 region (mask bit excluded), terminal NOP payloads, queue overflow
avoidance, and rejection of a valid draw followed by an unsupported command
without changing the framebuffer. These are synthetic fixtures, not captured
game frames or a comparison with real PS1 rasterization.

## LIBGS: preserve the game's table organization

| Call site / original entry | Required behavior | Next implementation |
|---|---|---|
| `func_80013154` in `main_services.c` | 320x240 initialization, side-by-side buffers `(0,0)` / `(320,0)`, four OTs per frame | Host setup adapter plus guest-layout descriptors |
| `GsClearOt` at `0x80085DB0` | Store zero-extended u16 offset/point into 32-bit fields; tag points at `org + 4*(2^length-1)`; reverse-clear | Implement against 20-byte guest descriptor; don't substitute the SDK's forward-list implementation |
| `GsSortOt` at `0x80085E10` | Splice source chain into destination at `source.point - destination.offset`, retaining payload length bytes | Recover and test exact tail-splice behavior before connecting `Graphics_BeginFrame` |
| `GsDrawOt` at `0x80085D80` | Draw from descriptor's `tag` field at +16 | Resolve that guest pointer, collect/validate, then submit GP0 snapshot |
| `Graphics_BeginFrame` | Conditionally splice secondary tables in order 1, 2, 3; draw; select next buffer; clear four OTs | Keep existing ordering and fade-mask decisions |
| `GsSetWorkBase` | Select the next 140,000-byte primitive arena | Preserve guest arena cursor and packet bounds |
| `GsInit3D`, `GsSetOrign`, coordinate/sorting calls | Initialization and later model transforms | Separate GTE/coordinate adapters; partial upstream LIBGS is insufficient |

`GraphicsFrameBuffer` has a 0x5110-byte tag area followed by four 0x14-byte
descriptors, for total size 0x5160. Initial lengths are 2, 6, 12, 6. The four
arrays start at tag offsets 0, 0x10, 0x110, and 0x4110. Host pointer-sized
descriptors would change both the offsets and the stride; preserve these layouts
explicitly at the guest boundary.

The retail `ClearOTagR` at `0x8007FBB8` installs a two-node tail at `0x80094728`
and `0x80094714`. The first has four command words and the second has four NOPs
with the final `0xFFFFFF` link. The initial first command is a VRAM-copy opcode.
Do not discard tail nodes merely because they belong to the SDK. `GsSortOt`
also traverses this tail, which makes a naive splice-at-terminator replacement
unsafe. Inspect runtime mutations before treating the initial words as constants.

## LIBDS: completion order is part of the loader

| Caller | Contract to preserve |
|---|---|
| `File_InitTransferState` | `DsInit` must succeed or report a host-visible failure; endless retry is not a useful native missing-disc path |
| `File_GetPosition` / `File_Exists` | ISO names, BCD locations and exact logical LBAs; `File_Exists` rejects both NULL and -1 pointer sentinels |
| `func_80014294.c` transfer state machine | Queued pause (9), set-filter (0xD), get-location (0x10), and combined `DsPacket` seek/read commands |
| `func_800140A0` and siblings | Event 2 completes a command; event 5 retries it; completion clears command-busy flags |
| `DsStartReadySystem` / `File_TransferReadyCallback` | Event 1 delivers sector readiness; callback can end ready delivery while consuming the last sector |
| `CdGetSector` / SPU upload / image upload | Data availability must precede the ready callback; the callback consumes and advances buffers in 2048-byte sectors |

Implement a bounded command queue and deliver completions at an explicit service
point. A ready callback may cancel the stream or enqueue another operation;
avoid inline reentrant completion when accepting a command. Preserve the
game's patched LIBDS contracts rather than assuming an SDK export with a similar
name has the same behavior. XA and movie modes require additional sector formats
and are separate from the initial ordinary-file read slice.

## Frame service integration

`Main_AdvanceFrame` orders `Main_RunFrameServices`, `Graphics_SyncFrame`,
`Graphics_BeginFrame`, then `Input_UpdatePads`. The VBlank callback increments
counters, reads raw pads, and runs sound synchronization under a reentrancy guard.
`Graphics_SyncFrame` busy-waits on a VBlank-updated counter **before** its `VSync`
call. A single-threaded port cannot implement callbacks only inside `VSync` and
leave that wait intact: it can deadlock. Replace the wait with the host scheduler
boundary while retaining counter/step semantics and service ordering.

`src/pc/compat/libgs_ot.c` now implements `ClearOTagR`, `GsClearOt`, `GsSortOt`
and the `GsDrawOt` collection step against 20-byte guest descriptors, using the
retail tail words read from the verified executable (`0x80094714`:
`0x04FFFFFF` plus four NOPs; `0x80094728`: tag, `0x80000000`, 0, 0,
`0x00010002`). `GsSortOt` walks from `org[0]`, not from `tag`; the node whose
link reaches the node before the terminator inherits the destination entry's
link, which removes the source's two tail nodes, and the spliced chain draws
before primitives already at that destination entry. The index is
`point - offset` with 32-bit wraparound. `pc_libgs_ot` covers this, an empty
source, and failure cases that must leave both tables untouched.

Next acceptance target: deterministic file-read completion and frame-service
adapters. Native main-menu integration still also requires guest globals,
relocations, overlay entrypoints, and asset initialization.
