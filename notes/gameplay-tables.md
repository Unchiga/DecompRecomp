# Gameplay tables: fusions, equips, rituals, drops and decks

A mod can change the duel's rule tables with no code at all: which cards
fuse and into what, what an equip card may equip, what a ritual needs and
makes, what each opponent drops, and what each opponent's deck is dealt
from. The rules sit in `mod.json` beside everything else, name cards the
way a person would, and combine with other mods' rules instead of
overwriting them. The worked example is
[`examples/mods/rule-tables`](../examples/mods/rule-tables/mod.json).

```json
{
    "id": "my-rules",
    "name": "My rules",
    "fusions": [
        {"with": ["Kuriboh", "Mystical Elf"], "result": "Celtic Guardian"},
        {"with": ["Baby Dragon", "Time Wizard"], "result": null},
        {"remove": "Gaia the Dragon Champion"}
    ],
    "equips": [
        {"card": "Legendary Sword", "add": ["Dragon"], "remove": ["Curse of Dragon"]}
    ],
    "rituals": [
        {"card": "Black Luster Ritual", "tributes": ["Celtic Guardian", "Dark Magician", "Mystical Elf"],
         "result": "Black Luster Soldier"}
    ],
    "drops": {
        "Simon Muran": {"pow": {"Blue-eyes White Dragon": 20}},
        "all": {"tec": {"Kuriboh": 0}}
    },
    "decks": {
        "Heishin": {"Dark Magician": 60, "Kuriboh": 0}
    }
}
```

Like the cards a mod adds, these are read once when the game starts, so a
mod with any of them needs a restart to apply or remove. Anything a rule
names that the game does not have (a misspelt card, an opponent that does
not exist) is reported in the Mods window and in `MEMORIES_TRACE=mods`, and
that one rule is left out.

## Naming cards

Anywhere a rule names a card it may use:

* the card's name as the disc spells it, `"Blue-eyes White Dragon"`; case,
  spaces and punctuation are ignored, so `"blue eyes white dragon"` finds it
  too (`notes/card-catalog.csv` lists every name);
* its number, `12` or `"12"`;
* a card a mod adds, by its stable identity, `"my-cards:moon-dragon:1"`
  ([More cards](more-cards.md)).

## Fusions

`"fusions"` is a list of rules:

| Rule | Effect |
|---|---|
| `{"with": [A, B], "result": C}` | A and B fuse into C, in either order. It adds a fusion the disc lacks, or changes one it has |
| `{"with": [A, B], "result": null}` | A and B do not fuse |
| `{"remove": C}` | no recipe from the disc makes C any more. Recipes from mods still do |

A rule for a retail card also holds for the copies of it a mod adds, as the
disc's table does; a rule that names a copy itself is surer, and comes
first. Mods' rules are asked before the recipes of an added card's own
`fusions` list, which come before the disc's table. The AI fuses by the
same rules as the player.

## Equips

`"equips"` is a list, one entry per equip card:

| Key | Meaning |
|---|---|
| `card` | the equip card |
| `add` | cards, or monster types (`"Dragon"`, `"Winged Beast"`), it may now equip |
| `remove` | cards or types it may no longer equip |
| `replace` | `true`: it equips only what `add` names, and nothing the disc listed |

Within one entry a named card is surer than a type and a type surer than
`replace`, so `"add": ["Dragon"], "remove": ["Curse of Dragon"]` equips every
dragon but one. What no entry mentions, the disc's table decides.

## Rituals

`"rituals"` is a list, one entry per ritual card. `card` is one of the
disc's ritual cards; `tributes` names the three monsters it takes, and
`result` what it summons. `"result": null` takes the ritual away. A tribute
may be a copy a mod added; a retail tribute is also met by a copy of it.

## Drops and decks

Each opponent draws its deck, and the card it gives when it loses, from a
weighted pool: a weight for each card, out of 2048. Four pools per
opponent:

| Key | Pool |
|---|---|
| `decks` | the cards its deck is dealt from (40 cards, at most 3 of each) |
| `drops` → `pow` (or `sa-pow`) | the prize for an S or A rank won on POW |
| `drops` → `bcd` (or `b-c-d`) | the prize for a B, C or D rank |
| `drops` → `tec` (or `sa-tec`) | the prize for an S or A rank won on TEC |

Opponents are named as in the table below, by their number, or `"all"` for
every one of them. A pool is an object of cards and weights:

* a listed card gets exactly its weight, out of 2048: `"Dark Magician": 60`
  is 60 chances in 2048, about 3%;
* `0` takes a card out of the pool;
* the cards not listed share whatever is left, in the proportions they had;
* `"replace": true` empties the pool first, so it holds only the listed
  cards;
* if the listed cards come to 2048 or more, or nothing else is left in the
  pool, the listed cards make up the whole pool, in proportion to their
  weights: `{"replace": true, "Kuriboh": 1, "Mystical Elf": 3}` is a
  quarter Kuriboh and three quarters Mystical Elf.

The weights are always brought back to exactly 2048, which the game's draw
needs. An edit that would leave a deck pool with fewer than 14 cards (a deck
of 40 at 3 copies each needs that many), or a drop pool with none, is
refused and the pool is left as it was.

Edits of the same pool from several mods add up: each applies to the pool
as the mods before it left it, so one mod making Blue-eyes likelier and
another taking Kuriboh out of the same pool both take effect. Edits apply on
top of what the game loaded, so a data mod's byte patch of a pool comes
first. `"all"` in a mod applies where it is written, before or after that
mod's edits of one opponent. Cards a mod adds may be in a pool too.

| # | Opponent | # | Opponent | # | Opponent |
|---|---|---|---|---|---|
| 1 | Simon Muran | 14 | Yami Bakura | 27 | Desert Mage |
| 2 | Teana | 15 | Pegasus | 28 | High Mage Martis |
| 3 | Jono | 16 | Isis | 29 | Meadow Mage |
| 4 | Villager 1 | 17 | Kaiba | 30 | High Mage Kepura |
| 5 | Villager 2 | 18 | Mage Soldier | 31 | Labyrinth Mage |
| 6 | Villager 3 | 19 | Jono 2nd | 32 | Seto 2nd |
| 7 | Seto | 20 | Teana 2nd | 33 | Guardian Sebek |
| 8 | Heishin | 21 | Ocean Mage | 34 | Guardian Neku |
| 9 | Rex Raptor | 22 | High Mage Secmeton | 35 | Heishin 2nd |
| 10 | Weevil Underwood | 23 | Forest Mage | 36 | Seto 3rd |
| 11 | Mai Valentine | 24 | High Mage Anubisius | 37 | DarkNite |
| 12 | Bandit Keith | 25 | Mountain Mage | 38 | Nitemare |
| 13 | Shadi | 26 | High Mage Atenza | 39 | Duel Master K |

Opponent 0 is an unused copy of Simon Muran. The disc's own pools are in
`notes/research/fusion-and-drop-tables/drops.csv`.

## Where two mods disagree

Mods apply in load order (priority, then `after` and `requires`, then the
order they were found; [the API 3 guide](mod-api-3.md)), and a later mod's
fusion, equip or ritual rule wins over an earlier one's for the same cards.
Pools add up, as above.

## Code mods

A code mod can decide the same questions at run time with managed events
([API 3](mod-api-3.md)): `FUSION` for fusions and `EQUIP` for equips come
before these tables, and a handled event overrides them. `REWARD` sees the
card a drop pool gave.

## How the port does it

`src/pc/cards/tables.c` reads the rules once, after the cards
(`Cards_Build`), from the mods in the order they loaded (`Mods_Loaded`).
Nothing the game loaded is changed; the functions that read each table ask
it first:

| Game function | Table | Asks |
|---|---|---|
| `Duel_CheckFusion` (`duel_card_checks.c`) | fusion table, `0x8017C2D8` | `Tables_Fusion`, then `Tables_FilterFusion` over the disc's answer |
| `Duel_CheckEquip` (`duel_card_checks.c`) | equip table, `0x8017A1D8` | `Tables_Equip` |
| `Duel_CheckRitual` (`duel_check_ritual.c`) | ritual table, `0x801799D8` | `Tables_Ritual`, whose recipe is laid out like the disc's |
| `Duel_ShuffleDeck` (`duel_shuffle_deck.c`) | deck pool, `0x801781D8` | `Tables_Pool(TABLES_POOL_DECK)` |
| `Duel_SelectCardDrop` (`duel_result_runtime.c`) | drop pools, `0x8017878C` | `Tables_Pool(TABLES_POOL_POW + pool)` |

A pool is worked out from the opponent's loaded pool and every edit of it
when the game draws from it, and kept until the opponent or the loaded pool
changes. An edited pool is drawn from exactly as the disc's is, with one
random number per draw and the same threshold, over every card the run has;
an opponent or pool no mod edits takes the game's own path, so without such
mods the random sequence, and every recorded run, is unchanged. The console
build has none of this (`#ifdef MEMORIES_PC`).

`tests/pc/tables_test.c` (ctest `pc_tables`) covers the rules, their order
between mods, the weights and the refusals.
