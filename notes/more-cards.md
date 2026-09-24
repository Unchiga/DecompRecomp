# More cards than the disc has

The PC port can have any number of cards past the disc's 722, up to card id
32766. A mod adds them with a `cards` list in its `mod.json`, and they work
wherever a card does: the Library, Build Deck, duels (the hand, the field, the
3D battle, fusions, equips, rituals and card effects), duel rewards, the card
viewer, saves, save states, trading and two-player duels.

Each new card starts as a **copy** of a retail card, its *base*. Model, effect
and fusion defaults come from that card, but entries can specify independent
`model`, `effect` and `fusions` values. Managed code hooks can implement new
behavior; see [Mod API 3](mod-api-3.md). Everything a player reads off the card
can be its own: name, artwork, card text, ATK, DEF, type, guardian stars,
level and attribute. What a mod leaves out is the base's. The disc has none
of these cards, so wherever the game goes to the disc, or to a table laid out
by the disc, it asks for the base instead, and lays the card's own art and
text over what comes back.

The release ships no card mod; the checks below were made with test mods
(a thousand made-up names, and a card with its own picture).

## The manifest

```json
{
    "id": "wacky",
    "name": "Wacky cards",
    "cards": [
        { "copy": "Kuriboh", "name": "Dingus Shmingus", "art": "images/dingus.png",
          "description": "A round and cheerful fellow who has never once been on time.",
          "type": "Beast", "attribute": "Fire", "level": 7, "stars": ["Moon", "Venus"],
          "attack": 2500, "defense": 2100 },
        { "copy": 1, "name": "Shmungus Mingus", "description": "Blue-eyes' cousin from out of town." },
        { "copy": "Kuriboh", "count": 100, "count_setting": "count", "name": "Kuriboh {n}",
          "drops": false, "opponents": true }
    ]
}
```

| Key | Meaning |
|---|---|
| `copy` | the base: a retail card id (1-722) or its name as the game spells it (`"Kuriboh"`, any case) |
| `count` | how many cards this entry adds (default 1) |
| `count_setting` | read `count` from one of the mod's settings instead, so `MEMORIES_MOD_<ID>_COUNT=5000` or `mod.<id>.count=5000` in the settings file changes it without editing the manifest |
| `name` | the cards' own name; `{n}` is the card's number within the entry and `{id}` its card id. Without one a card has its base's name. Letters, digits, spaces and ``!"#$%&'()*+,-./:<>?`` are what the game's font has |
| `description` | the card's own text, wrapped as the retail texts are (lines of up to twenty letters, broken at spaces; `\n` breaks a line where it stands). Eight lines is the most any retail text has. Without one a card has its base's text |
| `art` | a PNG in the mod (a path relative to its directory): the card's picture and, made from the same image, the small one the hand and field show. Any size: the middle of it at the card's shape is taken and scaled to 102x96 and 40x32, and its colours reduced to the 255 and 63 each has. 102x96 or a multiple looks best |
| `thumbnail` | a PNG for the small picture alone, when the scaled-down `art` does not read well at 40x32 |
| `title` | a PNG for the name plate at the top of the card's picture (96x14; dark ink on white, or on a transparent background). Without one, a card with its own name gets a plate with that name set in Times at the retail plates' size (Times New Roman on Windows, fontconfig's match for `Times` elsewhere, Liberation Serif on most Linux systems), or a blank plate when there is none |
| `attack`, `defense` | 0 to 5110, in tens, as the game stores them |
| `type` | a number or a name (`"Dragon"`, `"Winged Beast"`). A copy of a monster stays a monster, since it has its base's 3D model; a copy of a magic, trap, ritual or equip card keeps its type, since it has its base's effect |
| `attribute` | a number or a name (`"Light"` to `"Wind"`) |
| `level` | 0 to 12 |
| `stars` | the two guardian stars, as numbers or names (`"Mars"` to `"Venus"`) |
| `drops` | whether the card can be won in its base's place (default `true`, below) |
| `opponents` | whether an opponent's deck can be dealt it in its base's place (default `false`) |

What an entry leaves out is its base's. Give entries explicit stable `id` keys. Saves use these identities; runtime
IDs are remapped when mods change. Legacy numeric sidecars require explicit
migration as described in [Mod API 3](mod-api-3.md#card-definitions-and-identities).

The runtime ids follow each other in the order
the mods are found (sorted by directory) and the entries are written. A mod
with `cards` needs a restart to apply or remove, like a data override: the
cards are counted once, when the game starts. The window and the log
(`MEMORIES_TRACE=mods`) say which ids each entry got.

## Where a card comes from

The disc's reward and deck tables are of retail cards, and their weights add
up to 2048, so they are not extended. A copy that asks to (`drops`) is won in
its base's place instead: when a duel's reward roll lands on the base, the
game picks evenly among the base and every such copy. The base's family is
won exactly as often as the base was, and without such copies the game's
random numbers go exactly as before. `opponents` does the same for the cards
an opponent's deck is dealt. Starter decks and passwords are the disc's, so a
new card is never in a starter deck and has no password.

`MEMORIES_DEBUG_CHEST=3` (every card, three copies, new ones included) and
`MEMORIES_DEBUG_DECK="723-762"` (the deck, as ids and ranges) are the
development shortcuts; see [pc-build.md](pc-build.md).

## How it is done

The console build is untouched: every change to shared game sources is under
`MEMORIES_PC` or is a named constant that expands to the same number there,
and `make match` and `make match-overlays` reproduce the retail hashes.

**The count.** `CARD_COUNT` stays 722 everywhere the disc is laid out by it (the
art sectors are `(id - 1) * 7 + 722`). `CARD_COUNT_LIVE` (`gCard_nCount`) is
how many cards this run has, and bounds the loops. `CARD_TABLE_COUNT` (32766)
sizes the tables and workspaces indexed by card id
([`card_constants.h`](../src/game/card_constants.h)).

**The tables.** `gDuel_adwCardStats`, `gCard_asNameSortKey` and
`gDuel_abCardLevelAttr` are packed back to back in the executable with room
for 722 cards. The port defines them in
[`src/pc/game/card_storage.c`](../src/pc/game/card_storage.c), a game-side
unit, so the build stops pinning them to the retail addresses, and
`Cards_Build` ([`src/pc/cards/cards.c`](../src/pc/cards/cards.c)) copies the
retail tables in and appends the mods' cards at startup. The same unit holds
everything else that outgrew its console storage: the Library's per-card
display records (`D_800EA1E8`), the Build Deck workspace (two 0x6344-byte
panes at `payload_bases[0]` on the console, `BUILD_DECK_WORKSPACE`), the
trade screen's rows (`D_801845FC`) and the Library cursor, widened from `s8`.
Being game variables, they sit at fixed addresses inside every save state,
which is why a state is now about 7.5 MB.

None of the MIPS code the port runs under its interpreter (the WA effect
bank, the MODEL control modules) addresses these tables: a scan of the WA
bank and all of MODEL.MRG for `lui`-formed addresses found none in
`0x800EA1E8`-`0x800EAD88` or `0x801A0000`-`0x801E0000`.

**The base.** `Cards_BaseId` stands in for the card wherever the disc or a
disc table is behind the id: the big artwork (`func_80029164`), the small
field and hand images (`Duel_RequestCombinedDeckData` reads one sector per
unique *base*, and `Duel_PopulateCombinedDeckData` finds each card's by its
base), the 3D model (`Model_LoadMonsterMerge` from the Library, the battle's
`D_800EF658`: a copy's id could otherwise be 777, `MODEL_SPECIAL_BATTLE_ID`,
Exodia's), the card text (`duel_effect_command.c`), the fusion and equip
tables (`duel_card_checks.c`; an equip answers with the card it was asked
about), rituals (`Duel_CheckRitual`), card effects
(`DuelEffect_StartCardEffect`, so the handlers that test
`gDuel_wEffectCardID` see the base), traps (`Duel_SelectTrapByCardId`,
`duel_trap_resolution.c`) and the AI's damage-card values.

**Ids beside flags.** Build Deck's box rows and the placement result keep a
flag in bit 15 and read the id back with `& 0xFFF`; `CARD_ID_FIELD_MASK` is
`0x7FFF` on the PC port. Card ids themselves are 16 bits everywhere else.

**The trunk and seen marks.** The save's trunk is 722 bytes at +0x50 with 18
bytes to spare before the duelist code, and the Library's seen marks are
campaign flags `0x120 + id`, which reach the password flags at 0x400. So the
new cards' quantities and seen marks are kept by the port: `Cards_ChestSlot`
is the trunk byte of a card in a given save (the running one, the two a
two-player screen loads, or the copies a trade is made on), and
`Cards_Seen`/`Cards_MarkSeen` the seen mark. Every place that reached the
trunk directly goes through them: Build Deck's setup and exit, the Library,
`Library_CheckCardOwned`, `Duel_AwardCard`, the recent-drop compaction,
the trade screen and its commit.

**Beside the save.** A memory card block has no room for them either, so
`cards/<duelist code>.txt` in the user directory holds them, a section per
save sequence (`save <n>`, then `chest2 <identity> <count>`, `seen2 <identity>` and
`deck2 <slot> <old-id> <base> <identity>` lines, then `end`), the newest eight kept. The
game's own save writes one (`SaveData_RequestWrite`, under the sequence the
payload gets), a load reads the one for the loaded sequence
(`SaveData_PollLoad`), a two-player load reads both saves', and a trade
rewrites both once the memory cards took it. NEW GAME is noticed by the
running save's duelist code changing. A save made while no card mod was
applied has no section, and gets the newest earlier one: what the player had
when they last played with the mod. A deck that holds a card the run does not
have (the mod was removed) gets each such slot's base back, from its `deck2`
line, so a duel never deals a card that is not there. Ownership and seen records
for missing mods are retained; returning mods recover them by stable identity.

**Its own art and text.** The game still loads the base's art record
(`func_80029164`) and the base's thumbnail sector
(`Duel_RequestCombinedDeckData`); `Cards_PatchArtRecord`, called in
`func_800289BC` before the four uploads, and `Cards_PatchThumbnail`, called in
`Duel_PopulateCombinedDeckData` after each block is copied, lay the card's own
picture, plate and thumbnail over them
([`art.c`](../src/pc/cards/art.c) makes them from the PNGs: the record layout
is in its header comment and in
[modding-tutorial-evidence.md](modding-tutorial-evidence.md#card-image-editor-dimensions)).
The plate is drawn subtractively over the gold frame through its own fixed
palette: entry 1 takes the most away and is the darkest ink, 7 barely shows,
0 is clear. The retail plates put their stems at 1 with faint 6 and 7
fringes, and a plate that inks with 7 reads as a pale ghost. The generated
plates follow the settings the
[YuGiOhForbiddenMemoriesRecomp](https://github.com/yamyi/YuGiOhForbiddenMemoriesRecomp)
project measured against window captures (its `psx_card_packs.c`,
`render_title`): Times
regular at 13 pixels, the baseline under row 11, whole-pixel advances,
coverage in hard steps (150 and up ink 1, 96 an edge at 3, 40 a halo at 6),
and a name wider than 90 pixels squeezed into columns 3 to 93 and brought
back up to full ink. Card text goes in beside the name, at the text
engine's insert command (`duel_effect_command.c`, op 0x40).

**What spells out 722.** The Library's heading string (`"<seen/722>"`,
0x801B121D, text 0xF8) is replaced by the port's own for the real total, in a
box wide enough for it: nine 16-pixel letters fill the console's, and a
heading that wraps waits for a page press. Three-digit card numbers
(`F8 03` with width 3, Build Deck's list, the trade offers) grow to four or
five digits in the same room. Build Deck's list and the trade screen's scroll
end at the live count (`maximum = 715` was 722 - 7).

**The Library grid.** Its panels are sprite sheets (resource 0/3/n of the
Library package): a stone frame around two rows of big digits that spell
"001" over "100" to "701" over "722". The digit pieces only spell those
ranges (there is no 8 or 9 among them), so with more cards the sections from
701 on get a panel of the port's own
([`library_panels.c`](../src/pc/cards/library_panels.c)): the same frame from
the same texture page, with the plain stone band from between the retail
numbers where the numbers were, drawn through the game's sprite-sheet
renderer for the section rows on screen. The grid itself grows by section
rows of 200 (`CARD_GRID_SECTION_ROW_COUNT`).

## Limits

* Ids stop at 32766 (`CARD_ID_LIMIT`): card ids are signed 16-bit in the
  duel's records and 0x7FFF-masked beside their flags.
* A card defaults to its base's 3D model and effect, with independent `model`
  and `effect` borrowing available. Declarative recipes and code hooks extend
  fusion behavior; equip and ritual tables still use the base.
* The generated name plate is set in a system font, not the retail plates'
  own lettering; a `title` PNG replaces it.
* Copies of Exodia's pieces do not complete Exodia, and Build Deck's one-copy
  rule for the pieces is by id: a copy is another card.
* Legacy numeric sidecars require the original card mods and order for an
  explicit migration. See [API 3 migration](mod-api-3.md); new sidecars use stable identities.
* The Library's panels past section 7 carry no range numbers; the card number
  under the cursor is always shown.

## Checking it

With a save on the memory card (`tmp/morecards` has the harness used): the
Library heading, the grid's last section row and the card view (art, text,
model) of a new card; Build Deck at the end of the chest with 4-digit ids,
adding a new card, saving, and loading it back in a fresh process with the
counts intact; a Free Duel with a deck of new cards (hand images, names,
placement, a guardian star, a direct attack, the opponent's attack, and the
3D battle with a copy's model); a trade of a new card between two memory
cards (`MEMORIES_INPUT2` drives the second pad) with both sidecars rewritten
only after the write; a two-player duel; a save state taken in Build Deck and
resumed in a new process; 5,722 cards; a save with a new card in its deck
loaded without the mod; a card with its own picture, plate, text, type,
level, attribute and stars in the card view and in the duel's hand; and the
Windows build under Wine. Without a card mod
the smoke screenshots are unchanged on both systems.
