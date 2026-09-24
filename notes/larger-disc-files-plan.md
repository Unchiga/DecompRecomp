# Larger disc replacements — implementation plan

The first milestone is implemented in `src/pc/mods/mods.c` and
`src/pc/sdk/libds.c`: named-file growth, effective lookup, shared reads,
original-LBA compatibility, patch validation, startup preparation, restart
enforcement, texture provenance and save-state signatures. The implementation
keeps the map beside the existing override tables rather than adding another
owner for their lifetime. Synthetic-disc tests live in `tests/pc/disc_test.c`.
The resource descriptors and growing MRG-member loaders below remain future
work; no new `resources.models` manifest key is implemented yet.

One refinement from the draft: before code initialization, reserve the largest
successfully prepared candidate for each file, in retail-LBA order. Freeze
addresses before calling mod initializers. If an initializer fails, retain
that reservation and fall back to surviving content (or the physical file),
then revalidate patches and dependencies. This costs some virtual capacity
when replacements compete but preserves positions already cached by code.

Support larger named-file replacements through a virtual disc map, then add
resource-aware loaders for larger MRG members. These are separate milestones:
making extra bytes readable does not make the game request or safely consume
them. This proposal targets the PC port and leaves the player's image intact.

1. **What currently imposes the limit**

   [`apply_overrides()` and `add_region()`](../src/pc/mods/mods.c) resolve a
   named file to its retail LBA and sector-rounded size. A replacement can
   only cover that extent; an oversized replacement is mapped but its tail
   is ignored with a warning. Extending that range would replace sectors
   belonging to other files.

   [`read_raw()`](../src/pc/sdk/libds.c) applies overrides only after a
   successful physical read. Consequently, merely allocating replacement
   sectors beyond the image would still fail. `DsSearchFile()` also returns
   the directory's original position and size.

   MRGs have no generic member directory to rewrite: offsets, strides,
   transfer counts and callback phases live in game code and tables. For
   example, [`Model_LoadMonsterMerge()`](../src/game/model_load_monster_merge.c)
   requests `model * 0x114` through the next `0x114` sectors, then
   [`func_80056D7C()`](../src/game/model_texture_transfer.c) distributes the
   record through fixed phases. See [MRG structure](mrg-files.md) and
   [model destinations](high-memory-load-addresses.md).

2. **Virtual extents for larger whole files**

   Introduce a PC-owned map, provisionally `src/pc/sdk/disc_map.{c,h}`. Each
   entry records the normalized ISO path, retail extent and byte length,
   effective extent and byte length, backing data, and owning mod. Keep
   physical lookup separate from effective lookup so preparing an override
   cannot accidentally resolve an earlier override as the retail file.

   Preserve the current `data: [{file, replace}]` manifest shape. Files that
   fit retain their current placement and zero-fill behavior. A replacement
   exceeding the retail sector allocation receives a disjoint virtual extent
   after the physical image. Reserve complete 2048-byte sectors, report the
   replacement's exact byte size, and zero-pad its final sector. Keep the
   allocated extent at least as large as the retail extent for shorter
   replacements, preserving existing reads of their zero-filled tails.

   Allocate from the actual image length, not the last known archive.
   Resolve all enabled replacements before assigning extents, use a stable
   file ordering, and reserve the largest prepared candidate for each file.
   Leave a guard sector after each virtual extent so an accidental sequential
   overread fails instead of entering another virtual file. Unmapped virtual
   sectors are read failures, not successful reads of fabricated zeroes.

   `DsSearchFile()`, `Memories_DiscFileInfo()` and `Memories_DiscFileStart()`
   must agree on the effective position and size. Publish the map before
   `File_SetPositionTable()` populates `gFile_anLba` and before native mods
   cache file positions. Split data preparation from code-mod initialization
   if necessary to guarantee this ordering across the whole mod set.

   Keep a compatibility view at the original extent: original LBAs inside
   the file still read the corresponding replacement prefix. The relocated
   extent exposes the complete file. A read past the original extent still
   reaches the original neighbor; an absolute retail LBA cannot identify
   which file the caller intended. Callers needing the new tail must resolve
   the effective file position.

3. **One read path and explicit address limits**

   Route both drive callbacks and `Memories_DiscReadSectors()` through the
   same resolver. Resolve a virtual sector before attempting physical I/O.
   For virtual data, construct the raw-sector framing consumed by `libds.c`,
   including position, mode and duplicated data subheaders, with audio/video
   flags clear. Audit the callback and read-mode consumers rather than
   copying an arbitrary physical sector's headers. Preserve callback timing,
   read counts, texture-delivery tags and drive statistics.

   Initially support ordinary 2048-byte data files. Keep XA/STR replacement
   on their dedicated paths: a data-file mapping cannot provide their raw
   sector layout, channel metadata or playback semantics.

   Do not pick a huge sentinel LBA. `lba_to_loc()` and `loc_to_lba()` encode
   minutes/seconds/frames in BCD bytes. With valid two-digit minutes, the
   maximum representable LBA is 449849, including the 150-sector bias.
   Validate complete allocations and any one-past-end positions against
   that limit; use wide checked arithmetic before narrowing to existing
   integer fields. Also validate reported byte sizes and allocation sizes
   against the 32-bit guest's limits.

   This gives a bounded first implementation: usable extra space depends
   on the physical image and all active virtual extents. Exhaustion must
   reject the replacement with its required and available capacity, never
   truncate it. Supporting files beyond this range requires a subsequent
   PC file-handle/offset transfer path that bypasses BCD addressing; it
   cannot be delivered by changing one LBA constant.

4. **Patch composition, lifetime and compatibility**

   Resolve named-file patches as `(file identity, byte offset)`, then map
   them to both the effective extent and its original-prefix view. Validate
   against the final effective length so a patch can target the new tail.
   Preserve today's ordering: the last replacement in resolved mod order
   wins, then byte patches apply in resolved order. Report conflicting
   replacements and out-of-range patches against the selected final file.

   Raw `lba` entries retain their physical meaning and fixed `sectors`
   bound. Translate patches inside a relocated file's original extent to
   its matching logical offsets so existing tutorial patches reach both
   views. Split crossings at file boundaries; do not reinterpret a raw
   patch beyond the original boundary as a patch to the new tail. Reject
   oversized raw-region replacements rather than spilling or truncating.

   Prepare and validate each mod's contributions transactionally. On
   failure, remove that mod's candidate contributions and re-resolve winners
   and dependencies before publication. Never expose a partially built map.
   Mapping changes require a process restart even if a manifest specifies
   `restart: false`; cached LBAs and in-flight transfers make live relocation
   unsafe. Publish immutable tables outside interrupt context, and release
   backing storage only after the drive and its readers have stopped.

   Include the resulting layout, backing-content identity and resource
   metadata in save-state compatibility checks. Existing states contain
   drive positions and transfer descriptors. A mismatched mapping must be
   detected before restoring those fields; normal memory-card saves do not
   need a new format solely because files moved.

   Update texture provenance alongside relocation. Both original-prefix
   reads and virtual reads should tag the same effective file-relative
   bytes, and texture-pack resolution must use that same mapping. Rebuild
   cached archive starts when initializing a new map. For unchanged member
   layouts, existing archive-relative texture manifests should keep working.
   A repacked member needs explicit origin metadata or a matching new pack
   manifest; relocation alone cannot infer its old texture offsets.

5. **Larger MRG members need resource descriptors**

   Introduce PC-only descriptors for supported resource families, keyed by
   stable resource identity, such as a model ID rather than its compacted
   retail index. A descriptor supplies the source file/member, byte length,
   transfer phases and validated destination capacities. Missing overrides
   use descriptors derived from the retail layout.

   Prefer individual member replacement files over requiring a mod author
   to distribute a rebuilt `MODEL.MRG`. The disc map can back these members
   with virtual extents too, subject to the same capacity checks. A proposed
   manifest addition is `resources.models`, with each entry naming `id`,
   `replace`, and a versioned `layout` sidecar. Finalize that schema only
   after implementing and measuring the first model loader.

   Start with the ordinary duel model records. Replace the PC path's fixed
   stride/count calculation in `Model_LoadMonsterMerge()` with a descriptor
   lookup, then make its phase callback consume the same descriptor. Audit
   native bulk readers too. Changing just the starting sector or total count
   would leave the fixed phase destinations and lengths incorrect.

   Before enabling growth, inventory every affected destination, pointer,
   parser count, render buffer and cleanup path. Give larger data owned,
   guest-addressable storage where the consumers support relocation, and
   update those consumers together. Preserve fixed executable-module slots
   unless their calling and relocation contracts have been implemented.
   Validate phase totals, alignment, internal offsets, memory ownership and
   overlapping destinations before a transfer starts. Reject unsupported
   layouts with the specific limiting phase or buffer.

   The exact safe model capacities remain to be established by that audit;
   this draft does not assume ordinary host allocation makes embedded guest
   pointers or fixed module addresses relocatable. Keep retail matching
   behavior through existing PC build boundaries and test the retail build.
   Add WA/SU package families incrementally after their callback and memory
   contracts are known; there is no universal MRG repacker to enable here.

   Higher-resolution replacement PNGs already work through the
   [texture-pack system](modding.md#texture-packs-images-by-origin). Continue
   using that path for higher visual resolution. Changes to native texture
   dimensions also require validated upload geometry, palettes, UVs and
   VRAM placement, even after disc capacity is available.

6. **Implementation sequence and acceptance checks**

   Deliver three reviewable changes:

   - **Virtual named files:** map and effective lookup, common read path,
     patch semantics, startup publication, restart enforcement, provenance
     and save-state checks. Extend `tests/pc/mods_test.c` and add a synthetic
     disc fixture that exercises real `libds.c` lookup and drive delivery.
     No retail assets should be required for these automated tests.
   - **One growing resource family:** model descriptors, loader/phase
     changes and memory ownership. Demonstrate a valid model larger than
     its retail record actually consuming its added data, then unload and
     reload it repeatedly. Include both duel slots and an unmodified model
     as controls. Do not describe milestone one alone as larger-model support.
   - **Authoring and additional families:** descriptor validation tooling,
     example mod, supported-capacity documentation, and further recovered
     MRG package loaders.

   Required coverage includes a named replacement larger than its original
   extent; first/middle/tail reads beyond physical EOF; exact lookup size;
   final-sector padding and unmapped overread; unchanged neighboring files;
   identical results through bulk and callback reads; patches crossing sector
   boundaries and targeting the expanded tail; raw-patch compatibility;
   competing replacements and rollback; address exhaustion and arithmetic
   overflow; texture matching through both views; and save-state mismatch
   rejection before guest restoration. Check smaller/equal replacements and
   unmodified XA/STR playback for regressions.

   Finish with a PC smoke test of boot, menus, model loading and a duel, plus
   the project's required retail matching checks for touched game sources.
   Success means added bytes are requested, delivered and safely consumed
   by a supported loader—not merely that the size warning disappeared.
