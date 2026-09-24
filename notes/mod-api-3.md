# Mod API 3: settings, gameplay hooks, content and state

The existing portable ELF32 object format and API 1/2 layout remain supported.
API 3 appends services to `MemoriesModHost`; it does not enlarge `MemoriesMod`.
Check `host->api >= 3` before using them. Build code mods with
`python3 tools/pc/build_mod.py <directory>` and ship the object plus `mod.json`.
Both supported game builds consume the same object. Direct game-symbol access
remains available for advanced changes; it is coupled to the game build's
headers and implementation, unlike the managed event interface.

Worked examples are in `examples/mods/gameplay-rules` and
`examples/mods/card-pack`. They are not enabled or bundled automatically.

## Manifest and compatibility

```json
{
  "id": "my-rules",
  "name": "My rules",
  "version": "1.2",
  "author": "An author",
  "description": "What this changes.",
  "min_api": 3,
  "game": "slus_01411",
  "requires": [{"id": "base-rules", "min_version": "1.0", "max_version": "2.9"}],
  "after": ["visual-effects"],
  "conflicts": ["other-rules"],
  "priority": 10,
  "library": "rules",
  "settings": [
    {"key": "damage", "label": "Damage", "type": "int", "default": 100,
     "min": 0, "max": 300, "step": 10, "suffix": "%"},
    {"key": "enabled_rule", "label": "Extra rule", "type": "bool", "default": 1},
    {"key": "mode", "label": "Mode", "type": "choice", "default": 0,
     "choices": ["Classic", "Custom"]},
    {"key": "button", "label": "Action", "type": "key", "default": 1024}
  ]
}
```

IDs are 1–63 ASCII letters, digits, hyphens or underscores. `requires` accepts
IDs or objects with inclusive numeric dotted version bounds. `after` is an
optional ordering edge when both mods are enabled; `requires` also enforces
presence and activation. Mods caught in a cycle (and mods waiting on them) are
not loaded; the rest still are. Explicit conflicts are rejected
before applying a set. The user can override priority with `mod.<id>.order`;
lower numbers load first, but cannot bypass dependencies. Equal priorities
retain discovery order. `min_api` and `game` are checked before executing code.
An enabled dependency that fails initialization prevents its dependent from
starting during startup.

The loader accommodates 256 discovered mods. Named integer settings grow with
the collection instead of dropping entries after 128. Overlong keys or failed
allocation cause saving to fail rather than report a successful incomplete save.

Settings use `host->setting` as before. Keys use letters, digits, `_`, `-`;
`order` is reserved. `description` supplies help, and `restart: true` marks an
option that cannot be updated live. A `key` value is a normalized pad-button
bitmask (Select through Square), not an OS keyboard scancode; keyboard and
controller mappings are configured in Controls. Options are validated before
persistence. Missing defaults use zero; integer bounds default to 0–100.
Use integers in documented units for fractional quantities, e.g. thousandths.

Data replacements run before byte patches. Within either kind, later loaded
entries win. The manager warns when enabled mods target the same disc file/raw
starting sector, or when several texture packs may overlap. These are potential
conflicts, not a claim that arbitrary native code can be analyzed for conflicts.
Data override preparation is atomic per mod: one invalid entry rolls back every
prepared override from that mod. Warning text on a successfully active mod does
not make it permanently impossible to re-enable.

## Gameplay hooks

Call `host->subscribe(host, event, priority, callback)` during initialization.
It returns a token, or zero on failure. `unsubscribe` removes that mod's token.
Higher priorities run first; equal priorities run in registration order. A before
callback may edit the event's arguments. Setting `handled = 1` bypasses the
original operation and stops the remaining before callbacks. `result` supplies
its return value where applicable. All after callbacks observe the final result;
the dispatcher discards their edits to the event structure.

Same-event recursion bypasses subscribers, so a replacement can call the game's
original public function to get its normal behavior. Subscriptions added during
a dispatch begin with the next dispatch; removed subscriptions stop immediately.
Only active, successfully initialized mods receive callbacks. Failed initialization
clears both legacy callbacks and managed registrations. Disabling retains the
object and registrations for later reactivation; `applied(0)` remains the place
for a mod to undo resources or changes it owns.

There are at most 4096 subscriptions. Dispatch does not allocate. Input callbacks
run from the VBlank service, potentially in interrupt context: do no allocation,
file I/O, registration or other non-reentrant work there. Other callbacks run
synchronously at the game operation. Keep them short. `host->pad` reads normalized
input before managed input hooks, so inspecting it cannot recurse into a hook.

| Event | Fields and operation |
|---|---|
| `INPUT` | `a` pad port, `b` button bits; before can edit `b`, or replace with `result`; after receives the bits passed to the game |
| `DAMAGE` | `a` affected side, `b` damage, `c` 0 battle / 1 spell or reflected recovery; initial `result` is current LP; handled replaces remaining LP; after observes clamped remaining LP before the caller stores it |
| `REWARD` | `a` awarded card ID; edit it to replace the reward, or handle to cancel; invalid IDs are rejected |
| `FUSION` | `a`, `b` input card IDs before base-card mapping; handled `result` is the resulting card, or zero to forbid a fusion |
| `EFFECT` | Start: `a` presented card ID, `b` second-handler flag, `c=0`; update: `a` current effect card, `b` effect flags, `c=1`, `result` is returned flags. A custom multi-frame effect owns its flags and completion |
| `AI` | Wraps the final `func_800279BC` turn-action decision. A replacement owns the AI selection record and returns its decision in `result` |
| `SCENE` | Wraps `Main_ApplyMenuSelection`: `a` menu selection, `b` prior main mode; modify `a`, or handle the transition yourself |
| `SETTINGS` | After only: `a` manager mod index, `b` option index, `c` new value after applying the settings batch |
| `SAVE` | Before registered mod buffers are serialized: pack pointer-free state into your registered buffer |
| `LOAD` | After save-state restoration and legacy reset callbacks: rebuild runtime caches from the registered buffer |

These are concrete extension points, not an automatic replacement mechanism for
every exported function. Other game code can still be called or data edited via
the SDK; adding another managed event should include a documented call site and
regression coverage. Native mods run inside the game process.

## Card definitions and identities

Each card entry can have an `id`, local to its mod. Runtime IDs remain compact
numbers; persistence uses `mod-id:entry-id:n`, where `n` starts at 1 for entries
that generate several cards. Without an explicit ID the fallback is `entry-N`,
using the zero-based manifest index: keep entry order stable in such old packs.
Duplicate identities and invalid keys are diagnosed. Always give new entries
explicit IDs and keep them stable across releases.

`host->card_id(host, "example-cards:moon-dragon:1")` resolves a stable identity
after the startup card build. It returns zero before the registry is ready or
when the card is unavailable. Names and artwork are presentation, not identity.

An entry's `fusions` list accepts `{ "with": <retail-id-or-stable-identity>,
"result": <retail-id-or-stable-identity> }`. A result of zero forbids that
combination. Explicit recipes are checked before base-card fusion tables.
`model` and `effect` independently choose retail model/effect IDs, rather than
forcing both to match `copy`. Managed effect/fusion hooks can implement behavior
that has no retail counterpart. New arbitrary 3D geometry still requires a code
mod or a replacement of the corresponding model data; `model` is a retail ID.

New sidecar sections persist identities for ownership, seen flags and decks.
Changing discovery order remaps the same cards. A missing card falls back to its
retail base in a deck; its stored ownership/seen flags survive saves made with
other card packs, and return when it is reinstalled.

Old sidecars have numeric IDs only. By default their ambiguous ownership is not
assigned to current cards, decks fall back to the recorded retail base, and the
old numeric lines are carried over unchanged (a copy is kept as `.txt.legacy`)
while new progress is still saved under stable identities. To migrate, restore the **original card mod set
and entry order**, launch once with `MEMORIES_MIGRATE_CARD_IDS=1`, load the save,
and save again. Retained sections are then converted to identities. The game cannot infer a lost historical
mod order; keep that backup if the original set is uncertain.

## Save states

`host->register_state(host, buffer, size, version)` registers one block per mod,
at most 1 MiB. Keep the allocation alive until shutdown; store values/indices,
not pointers. Bump the version whenever the layout changes. A failed registration
returns zero. Use SAVE/LOAD events for packing or rebuilding richer state.

Before changing game memory, loading checks the active mod identities, manifests,
loaded code hashes, actual activation order, effective declared options and stored
extra mod settings, plus each registered buffer's size and version. A mismatch
rejects the state and asks for its original profile. Old vanilla states remain
usable; old modded states without a compatibility record are rejected. This does
not hash every external texture or data asset: bump the manifest version when
changing shipped assets. Normal memory-card saves use identity remapping instead
of requiring an identical active mod set.

## Validation

`pc_mods_manager`, `pc_mods_window`, `pc_cards_identity`, `pc_settings`, and
`pc_mods_lifecycle` cover the new paths. The lifecycle script runs actual rejected
native objects on Linux and Windows/Wine; its Linux half also checks the real
save-state chunk reader and compatibility preflight. The ordinary loader fuzz
fixtures and deterministic game smoke cases remain in place. PC-only hooks are
excluded from retail compilation; `make match` remains the exact-match gate.
