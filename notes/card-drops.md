# Card drops

**Game > Card drops** sets how many cards a won duel deals: 1 (the console's)
to 99. Drag the slider, or open the menu with the keyboard and step it one at
a time with Left/Right. The setting is saved as `card_drops`;
`MEMORIES_CARD_DROPS=N` sets it for one run.

## On RESULTS OF DUEL

The SPOILS page is the game's own: it shows one card, which the game awards
as always. The other cards get pages of their own, between SPOILS and the
statistics:

SPOILS → drops page 1 … drops page N → statistics → SPECIAL ARTS → SPOILS

Left goes the other way. With one card, or after a duel with no reward, only
the game's three pages remain.

The added pages use SPECIAL ARTS' seven plates and the game's text box, so HD
text and mod fonts apply to them too. Each plate is one card: its number, its
name (cut with `...` when it would run into the columns), `xN` for more than
one copy, and **NEW** when you had no copy of the card in your deck or chest
before the duel. New cards come first, then the rest, each by number. The
heading gives how many cards the pages hold and which page of how many this
is.

The card SPOILS shows is never listed again. A page can still show the same
card when the table dealt another copy of it, since that copy is a separate
card.

## How the cards are dealt

Every card comes from the game's own roll, `Duel_SelectCardDrop`, on the
table SPOILS uses (the opponent and your rank), including tables edited by
mods and mod cards that replace retail ones. At 2 or more the random numbers
are drawn in the community drop mod's order: one draw thrown away, then for
each card six draws and the roll, `1 + 7N` in all. The last roll is SPOILS'
card. For the same seed you get the same cards as on that mod's patched discs
(the static recomp's MODS > CARD DROPS matched this order too). At 1 the game
makes its single roll, as on the console.

The cards are awarded when you leave the screen, all but SPOILS' card first,
through `Duel_AwardCard`, so mods see `MEMORIES_EVENT_REWARD` for each card.
SPOILS' card is awarded last and is the newest of the chest's 16 recent cards
(its New! marks). A card already at the chest's 250 stays at 250.

## Save states

The dealt cards and the page on screen are game variables
(`src/pc/game/drops.c`), so a state saved on RESULTS OF DUEL keeps them:
loading it shows the same pages and awards the same cards, whatever the
setting is by then. The setting only changes the next duel.

The file sorts after `card_storage.c` on purpose. Game units link in name
order, and a variable inserted before existing ones moves them, which makes
states from earlier builds fail to load ("game variable ... moved").

## Code

- `src/pc/cards/drops.c`, `drops.h`: dealing, awarding, page order and
  the page text.
- `src/game/func_800218F0.c` (`DuelScene_UpdateResultRewards`): calls
  the roll, the award and the page turn, under `MEMORIES_PC`.
- `src/game/duel_result_runtime.c` (`Duel_ShowResultPage`): an added page
  takes SPECIAL ARTS' plates (resource variant 2) and string
  `CARD_DROPS_TEXT_ID` (0xFFFF), which `Text_Resolve` answers with the
  composed page.

The page uses the text codes the result strings use: `f8 01 dy` (new line
`dy` pixels down, signed), `f8 06 x16` (x), `f8 02 dx`, `f8 04 1|2` (the
small or large letters) and `f8 0A` colour. Spaces are `f8 02 08` steps, not
glyphs. The text box can hold 255 glyphs, and a page keeps under 240.

## Verification

With the disc in `game/` and the game built, `python3
tools/pc/test_card_drops.py` wins a real duel (the smoke opening, then the
opponent's life points zeroed in a saved state) and checks that:

- with one card, Right from SPOILS reaches the statistics and one card is
  awarded;
- with twenty, Right opens the first added page, Left from SPOILS reaches
  SPECIAL ARTS, and Left from the statistics reaches the last added page;
- leaving awards twenty cards, from a state saved on the screen and with
  the setting set back to one, with SPOILS' card the newest.

`--windows` runs it on the Windows build under Wine. Screenshots and frames
are left in `tmp/pc/card-drops-smoke/`.
