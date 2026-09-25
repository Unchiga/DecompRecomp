# Fusion helper

Turn on **View > Fusion helper**. It is off by default; the setting is saved,
and `MEMORIES_FUSION_HELPER=1` also enables it.

The panel names the best card the hand can make (by ATK), with its ATK/DEF,
and numbers the cards to pick for it. The line is white with no picks or
one of its materials picked; green when the picks make it now; red when
they make anything else (including two of three materials that fuse into
something in between) or when a first pick is not a material. Numbers stay
on the remaining cards while the card can still be made. From two picks on,
a **Result** line under it shows what going ahead now gives. Equal results
use fewer cards, then DEF, then leftmost pick order.

The helper follows the human-controlled hand, including the second player in
local two-player duels. It hides during other phases, card inspection and
quit dialogs. It searches all ordered subsets of the five hand slots; two
copies of a card remain separate materials. It never plays cards for you.

Text is rasterized in window pixels through FreeType, independently of the
internal game resolution, and uses translation/mod fonts. The see-through
panel sits in the empty strip above the FIELD box, left-aligned
with it, so it never covers the zones or the hand. Pick-order badges sit in
each hand card's top-right corner (the game's selection tags use the
top-left) and follow the actual hand objects. This works
with the SDL/OpenGL and SDL fallback presenters and the legacy X11 backend.
Small windows clip long names with an ellipsis. No additional assets needed.

## Rules and mods

The preview and gameplay share `CardRules_Fusion` and `CardRules_Equip` in
`src/game/duel_card_checks.c`. Queries read the current disc tables, manifest
fusion/equip rules and card definitions, including cards beyond id 722,
base-card inheritance, removals and load-order overrides. Retail's odd-count
fusion table behaviour (including accidental fusions) is preserved.

The planner models fusion before equip, either equip order, accumulated
bonuses, loss of those bonuses when a new fusion monster is created, and the
monster surviving an incompatible spell/trap. Megamorph's 1000-point bonus
uses its actual id 657, as native placement does; copies get 500 unless the
game's placement code is changed too. Zero printed stats receive bonuses.
ATK/DEF include terrain and existing card modifiers, clamped to 0–9999.
Guardian-star matchup bonuses and later spell/trap effects are not predicted.

Previewing must not emit gameplay events, consume random numbers or modify
the duel. If an applied code mod subscribes to fusion/equip events or hooks
the corresponding lookups, placement or stat/terrain calculation, the helper
shows **preview unavailable for custom rule code**. It cannot safely predict
arbitrary stateful code. Data/table/card mods need no extra integration.

This ports the in-duel assistant concept and pick-order behaviour from the
static recomp's `psx_fusion_assist.c` / `psx_fusion_overlay.c`. The standalone
fusion database editor is a separate feature and is not included here.

## Verification

`pc_fusion` tests the planner, selected prefixes, duplicates, failed
combinations, both rankings, equip order/stacking/reset, terrain and clamps.
`pc_fusion_rules` exercises packed table bytes (including odd groups), added
cards and rule overrides, compares all 320,400 unordered pairs over its
800-card fixture through gameplay and preview, and checks that preview emits
no gameplay events. Run them with:

```sh
cmake -S . -B tmp/tests -DMEMORIES_SANITIZERS=ON
cmake --build tmp/tests -j
ctest --test-dir tmp/tests --output-on-failure
```

With the user's disc in `game/` and the Linux SDL game built,
`python3 tools/pc/test_fusion_helper.py` runs a real duel, picks and summons
both a retail fusion and an added mod card, checks visibility and unchanged
card/RNG state, and saves window screenshots under
`tmp/pc/fusion-helper-smoke/`. It uses SDL's offscreen driver by default.
