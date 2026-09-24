# Translations

A mod can put the game in another language: every line of dialogue, every
menu string, every card's name and text, the monster types, the guardian
stars and the duelists' names. Accented letters work (é, ñ, ç, ü, ø, ß,
¿, ¡ and the rest of the Latin alphabets), and a mod can bring a font for
anything else. Text drawn as pictures (the main menu's words, the results
screen's headings, the name plates on card art) is not text to the game;
a [texture pack](modding.md) repaints those.

## Making one

1. Write the game's text out of your own disc:

   ```sh
   python3 tools/pc/text_listing.py extract -o text.txt
   ```

   It finds the disc image in `game/` (or name it with `--exe`). The
   listing is plain UTF-8, about 10,000 lines.

2. Translate it in any text editor, keeping every `{...}` code and every
   `[ID]` and `{:L...}` line (see below).

3. Make a mod of it:

   ```json
   {
       "id": "spanish",
       "name": "Español",
       "description": "The whole game in Spanish.",
       "text": "text.txt"
   }
   ```

   `text` may be a list of files; they are read in order, and a later
   string with the same id replaces an earlier one. So may `font` (below).

4. Put the mod's folder in `mods` in the user directory, apply it in
   **Game > Mods** and restart. What the game could not read, and letters it
   has no way to draw, are listed beside the mod in the Mods window and in
   `MEMORIES_TRACE=mods`; everything else is used.

A translation may be partial: strings it leaves out stay as they are, and a
jump to a place the file does not have lands in the game's own text. The
listing itself must not be shared as it comes out of the tool: it is the
game's text. Share your translated file.

## The listing

```text
@bank dialog

[0501]
My dear prince!
Are you going to the city
to play cards again!?{page}You are of royal blood!
...{choice 02}«Run away»
«Keep listening»
{choose 80 L15DE L140F}

{:L140F}
The Pharaoh has gotten
wind of your activities...
```

* `@bank dialog`, `@bank descriptions` and `@bank names` start the three
  banks of the game's text: dialogue and menus; card texts; and card
  names, monster types, guardian stars, duelists and places.
* `[ID]` starts a string: its number, in hexadecimal. Its text starts on
  the next line. Card `n`'s name is `8000 + n` and its text `D100 + n`
  (`[8001]` and `[D101]` are Blue-eyes White Dragon's); the descriptions
  carry the card's name as a comment.
* A line break is a line break in the game's text box. The text box does
  not wrap by itself: break the lines where they fit. Card texts have
  lines of 20 letters and room for 8 lines.
* A string ends at `{end}`, or at a code that jumps away (`{jump}`,
  `{choose}`, `{f8 17}`, `{f8 18}`, `{f8 28}`). What follows it up to the
  next `[ID]` or `{:L...}` is ignored, so blank lines and `# comments` can
  go there.
* `{:LXXXX}` on a line of its own is a place something jumps to: a
  choice's answer, a branch, a shared ending. Its text belongs with it; keep
  the line. The listing names each after its place in the game's own text.
* `{cont}` marks a string that runs on into the next item without ending;
  keep the two in that order.

The codes:

| Code | What it is |
|---|---|
| `{page}` | wait for the button, then a fresh box |
| `{nl}` | a line break where the listing cannot write one (before a new item) |
| `{sp}` | a space at the end of a line, which an editor would strip |
| `{choice NN}` | the choice that follows: the next lines are its answers, one per line |
| `{choose 80 La Lb ...}` | where each answer goes, in order |
| `{jump L}`, `{call L}` | go on at `L`; insert the text at `L` (`{call L125A}` is the player's name) |
| `{if FFFF L}`, `{set FFFF}` | go to `L` if a story flag is set; set one |
| `{state ...}`, `{fx ...}` | pictures, pauses and effects of the story |
| `{f8 ...}` | the text's formatting and inserts: `{f8 00 20}` the card's name, `{f8 00 40}` its text, `{f8 03 ...}` a number, `{f8 0A NN}` a colour, `{f8 01 NN}`/`{f8 02 NN}`/`{f8 06 ...}` positions, `{f8 0E ...}`/`{f8 10 ...}` music and sound |
| `{g NN}` | a glyph by its number: the few symbols with no character to type |

Move a code with the words it belongs to; do not change its numbers. The
retail glyphs can be typed as themselves: letters, digits, `! " # $ % & '
( ) * + , - . / : ; < = > ?`, `«` `»`, `·`, `α β γ`, `← →`, `♂ ♀`; typographic
quotes and dashes are taken as their plain ones.

## Letters

The game's font has 91 letters, none accented. The port draws more,
the first time a text uses them:

* **An accented letter** is the retail letter with its mark drawn on, in
  both text sizes: acute, grave, circumflex, diaeresis, tilde, ring,
  cedilla, caron, macron, breve, dot, double acute and ogonek, on any letter
  Unicode combines them with (251 of them: á, Ž, ő, ę, ǎ, ẽ...). A
  capital with a mark above is set a little shorter so the mark fits the
  line. Also ¿ ¡ ı ø Ø ł Ł đ Đ ħ Ħ.
* **ß ẞ æ Æ œ Œ ð Ð þ Þ º ª ° €** are built in, drawn from Noto Sans Bold
  (SIL Open Font License) and given the retail letters' outline and
  shading.
* **Anything else** (Greek, Cyrillic...) is set in a font: first the
  mod's `"font"` files (`.ttf`, `.otf`), then the system's sans-serif.
  Ship a font with the mod if it needs one: the system's differs between
  machines, and a machine may have none.

The added letters live in the software GPU's texture bank 15, not in the
console's VRAM, and take their colours from the text's own palettes, so they
fade, flash and change colour as the retail ones do. Up to 672 of them.

## How the port does it

The text is data in the game's executable: three banks at `0x801B0000`
(menus and dialogue), `0x801C0000` (card texts) and `0x801D0000` (names),
each string found by its id through a table. Their bytecode is described in
[the text control codes](text-control-bytecode.md); `text_listing.py`
decodes all of it, following every jump from every string, and `check`
assembles the listing again and compares it with the retail bytes (all
128,166 of them match).

At startup `src/pc/text/translation.c` compiles each mod's listing
(`listing.c`) into a buffer of its own. The game turns a string id into
text in four places (`TextBox_BuildStep`, `Text_LookupString`, the
card-name and string inserts in `duel_effect_command.c`), which ask
`Text_Resolve` first. A jump in the game's text replaces the low 16 bits of
the text pointer, which only works inside a 64 KB bank; the compiled text
is anywhere and any size, so its jumps are indices into a table of its own
places, and the five jump handlers (`{jump}`, `{call}`, `{if}`, `{choose}`,
`{f8 17/18}`) ask `Text_Retarget`. A place the listing does not define is
the retail address, which is how `{call L125A}` still reaches the name the
game writes there.

Glyph codes above the retail ones (`0x100` on, written `F1`-`F5` and a low
byte, which the game already reads as a glyph) have words of their own
(`Glyphs_Word`), and `func_80035E20` draws them from bank 15
(`Glyphs_Cell`). If a translation renames cards, their alphabetical order
(`gCard_asNameSortKey`) is worked out again from the new names, accents
sorting as their plain letters. The names and texts of cards a mod adds
([more cards](more-cards.md)) are UTF-8 too and take the same letters.

Running the recorded smoke cases with the untranslated listing installed
as a mod gives the same pictures as without it, byte for byte, jumps into
the listing's own text and to the player's name included.
`tests/pc/text_listing_test.c` (ctest `pc_text_listing`) covers the
compiler.
