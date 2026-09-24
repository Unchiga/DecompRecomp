# AI Hard Mode

An optional API 4 code mod for SLUS_01411. Enable **AI Hard Mode** in the Mods
window. It ships disabled so installing an updated game does not silently
change existing duels. Linux and Windows use the same `ai-hard-mode.o`.

The default configuration improves decisions while retaining each opponent's
original deck-access limit and hidden-card knowledge. All options are in the
Mods window and apply to subsequent decisions.

## Options

| Setting | Behavior |
|---|---|
| Plan hand plays | Score single cards, field upgrades, fusion chains and useful spell plays. Off uses the original hand script. |
| Search fusion chains | Include fusion and equip chains in planned hand plays. Requires Plan hand plays. |
| Choose guardian stars | Compare both stars for new monster plays, including when using the original hand script. |
| Plan attack sequences | Search attack orders, preserving stronger attackers when an economical attacker suffices. Check winning sequences before fixed wall-card defense. |
| Situational defense | Compare the predicted damage and loss in attack versus defense. Off retains the original wall/position policy. |
| Smarter spell timing | Evaluate burn, healing, removal, equips, terrain, Swords, reveal, Stop Defense and available rituals. Unknown effects stay set. |
| Use effective stats | Use current field modifiers and project terrain onto candidate monsters. Off scores field cards using their printed stats. |
| Allow equal-power trades | Include equal-ATK trades when the sequence is valuable. |
| Preserve valuable hand cards | When using upcoming deck cards, exchange the least valuable unselected held card instead of the first slot. |
| Deck access | Original opponent limits, five-card hand only, or a custom window of 5–20 cards. The window includes the actual hand. |
| Hidden-card knowledge | Original opponent knowledge, no hidden stats, or knowledge of hidden stats for every opponent. |
| Maximum hand materials | 2–5 hand cards in a planned combination, optionally with an existing field monster. |
| Search budget | Caps fusion probes and attack-search nodes. Default 6,000; maximum 20,000. Small spell comparisons use separately bounded searches of at most 128 nodes each. |
| Face-down attack risk | A heuristic willingness setting, not a random-roll percentage. Zero avoids blind attacks; higher values lower the attack-strength threshold. |
| Material preservation | Penalty per hand card spent; higher values prefer shorter combinations. |
| Log AI decisions | Logs opponent, phase, selection, utility score and main search-node count on the `mods` channel. |

Hand planning and field planning are independent. With a field subsystem
disabled, the retail script supplies that part of the decision. Turning all
tactics off is not an exact retail replay unless guardian selection, hand
preservation, and information overrides are also returned to their original
settings. Disabling the whole mod removes its hooks.

The planner is deterministic for the same board and options. It does not
draw from the game's RNG. The retail fallback still uses its original RNG.

## Behavior and limits

The mod fills the existing twelve-byte AI selection record. The normal game
executes card placement, fusions, effects, position changes and battle, then
requests another decision. It does not grant extra actions or create cards.
Deck access selects existing upcoming cards and swaps displaced hand cards
back into those deck positions. Windows are clamped to real available cards;
stale snapshot tails are cleared before the original scripts can search them.

Hidden opponents are redacted before metadata lookup in the new planner.
It knows an unknown card occupies a slot, but receives no card ID, stats,
type or guardian star. The original scripts' visibility-aware searches are
also hooked so information settings apply when a subsystem uses retail logic.
Blind attacks use a fixed risk heuristic and are not treated as guaranteed
removals leading to a winning sequence.

Attack search considers every legal root candidate and up to three promising
continuations per state, to at most five attacks. Fusion search uses iterative
material limits and a probe budget. These are bounded heuristics, not an
exhaustive solver or a search across future turns. Hand scoring also values
immediate matchups and survival; it does not run a full attack search for
every fusion candidate. No win-rate improvement has yet been measured.

Card definitions, fusion recipes and equip compatibility use the running
game's rules, including table/card mods. Effect evaluation models the retail
effects identified by `Cards_EffectId`. Arbitrary replacement effects, damage
multipliers and stateful fusion/equip callbacks cannot be fully predicted.
Compatibility callbacks may be queried repeatedly during planning and should
not mutate game state merely because a hypothetical pairing was queried.
Equip estimates use the retail +500 / Megamorph +1000 bonuses.

The implementation keeps no decision cache or pointers in saved game state.
It reconstructs each decision from the current VM phase and board, so loads
and mod reactivation do not rely on a stale search. API 4 owns hook removal
and chaining. It does not alter the opponent parameter table or bytecode.

## Build and checks

```sh
python3 tools/pc/build_mod.py mods/ai-hard-mode --out tmp/pc/mod-build/ai-hard-mode
python3 tools/pc/test_ai_hard_mode.py --target both
python3 tools/pc/smoke_ai_hard_mode.py --target both
python3 tools/pc/smoke_ai_hard_mode.py --target both --retail-tactics
```

The regular game packaging step includes this directory automatically.
The portable planner is also the `pc_ai_hard_mode` CTest, supporting the
repository's address/undefined sanitizer option. The i386 adapter fixture
checks real guest layouts, hand/deck exchanges, hidden-data redaction,
remaining-deck bounds, Swords ownership and retail fallback without rendering.
The runtime smoke requires built Linux/Windows packages and local disc data;
it checks completed AI turns and a selection from the upcoming deck. Windows
checks use Wine when run on Linux. Add `--sanitize` to the fixture runner for
Linux address/undefined sanitizer coverage.

For runtime diagnosis, enable Log AI decisions and launch with
`MEMORIES_TRACE=mods` and optionally `MEMORIES_LOG=<path>`.

See [the research](../../notes/ai-hard-mode-research.md) for the retail turn
flow, all 39 opponent profiles and known limitations of the original AI.
