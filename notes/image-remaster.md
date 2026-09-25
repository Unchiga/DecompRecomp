# Image remaster: how an HD pack is made, and what went wrong

This is the record of the first attempt at an upscaled image pack for the PC
port (September 2026). The engine side works and is verified: a pack
replaces exactly the texels it names, in software and in OpenGL, at the
console's resolution and at Internal 2x/4x. The pack itself, made with an
AI upscaler (Upscayl's `high-fidelity-4x`), **was judged too ugly to
publish** and is not distributed. Everything below is here so that whoever
tries again starts from what is known instead of rediscovering it.

Related notes: `notes/modding.md` ("Texture packs") for the mod format,
`notes/pc-build.md` ("Images from the disc", "Texture dump", "Internal
resolution") for the tools and switches, `notes/mrg-files.md` for the
screen packages the extractor replays.

## The pipeline

1. **Extract** the game's images from the disc, named by origin:

   ```sh
   python tools/pc/extract_images.py --out tmp/pc/images-cp cards portraits
   python tools/pc/extract_images.py --out tmp/pc/images-sheets \
       --variants <dump>/assets.txt [--variants ...] sheets scenes
   ```

   Families (all replay a loader from the decompiled C, nothing is guessed
   from the screen):

   | Family | What | Loader |
   |---|---|---|
   | `cards` | 722 card arts 102x96, the name strip, the side strip, the 40x32 thumbnail (`NNNN.small.png`) | `func_800289BC`; the thumbnail is the card's own WA sector `n-1` |
   | `portraits` | 48x48 dialogue portraits, campaign 25 + Free Duel 40 | `Campaign_LoadScenePackage`, `FreeDuel_Init` |
   | `sheets` | every screen package streamed through the GPU path: main menu (SU.MRG), boot UI and title, story UI, campaign, Free Duel, name entry, password, options, game over, duel results and rewards, Library, the seven terrains, the map's strip, 65 display-effect records | the stage callbacks listed in `notes/mrg-files.md` |
   | `scenes` | the story's pictures: 33/81/113-sector records from WA sector `0x21D5` (backgrounds, the card shop, characters, cutscenes) | `ScriptImage_RequestTransfer` |

   A *sheet* is one 64-word column (0x8000 bytes, 256 rows) of a package
   and one *reading* of it: depth and palette. The game reads the same words
   with several palettes, so each reading is its own PNG.

2. **Capture dumps** (`MEMORIES_DUMP_TEXTURES=<dir>`, from a cold boot):
   `assets.txt` lists every texture drawn with its disc offset, size,
   depth, palette and pixel crop. Dumps are needed for two things the disc
   alone does not say: which palettes a screen uses on a sheet
   (`--variants`) and where the game cuts pieces out of a sheet (`--cuts`,
   below). See "Reaching every screen" for capturing without a player.

3. **Upscale** into a mod folder:

   ```sh
   python tools/pc/upscale_pack.py --images tmp/pc/images-sheets \
       --images tmp/pc/images-cp --out mods/<pack> \
       --cuts <dump>/assets.txt [--cuts ...] [--zip tmp/pc/<pack>.zip]
   ```

   `--merge <pack>` adds an already-made pack as is (hand-painted images,
   another model's output). `--scale` (default 4) and `--max-side` (2048)
   bound the size. Runs into the same `--out` accumulate.

4. **Install**: the mod folder goes in `mods/` (the build copies it to
   `tmp/pc/game32/mods`) or in the user's
   `Documents\My Games\YFM Re-Decomp\mods`. Keep packs out of git
   (`.git/info/exclude`): the images are the game's.

5. **Check** with the oracle, not by eye alone: a pack of the extracted
   PNGs *as they are* (no upscale) must draw the same frame as no pack,
   0 pixels, at 1x and 2x, software (`MEMORIES_GL_PICTURE=0`) and GL. A
   copy with red and blue swapped shows which texels the pack actually
   reaches. Then compare GL against software with the real pack.

## Problems met, in the order they showed up

Each one cost a round of testing; the fix is in the code unless said
otherwise.

1. **Crash when a pack loaded** (addresses at multiples of 2352): the
   delivery copies were allocated only by the dump, not by the tags a pack
   enables. `TextureDump_EnableTags` allocates both.

2. **A glow became an opaque disc.** A replaced texel took the pack's
   colour and lost the word's semi-transparency bit. The pack replaces the
   colour; the bit always comes from the original word (both renderers).

3. **25x made the game crawl.** The game keeps every pack image in memory
   at full size, and each one is also a GL texture; a 3200x3200 image is
   40 MB. 4x is the default; `--max-side` caps a single image. Neither the
   decoded images nor the textures are ever freed (a later improvement).

4. **Only one pack worked at a time.** Loading a second pack replaced the
   first. Packs of all enabled mods now add up, and disabling one reloads
   the others.

5. **Text and numbers turned into noise** with dump-cut entries: many tiny
   overlapping sub-rectangles (glyphs cut from one font sheet) cannot be
   told apart by the lookup. Dump assets below 32 pixels are skipped
   (`--min-size`); sheets replaced dump assets altogether.

6. **A sheet read with another palette showed the wrong colours** (the boot
   UI's colour ramps, a terrain's palettes). A pack now holds one entry per
   reading of the same words; `prepare()` in `texture_pack.c` picks the one
   whose palette the primitive uses. At 1x only the first reading shows
   (the shadow VRAM holds one set of colours); the scaled picture shows all.

7. **The "1:1 pack" was not 1:1** (thousands of pixels off by one level):
   the extractor rounded 5-bit channels (`c*255/31`) while the renderers
   use `(c << 3) | (c >> 2)`. The extractor and the dump now expand like
   the renderers.

8. **A vertical line every 64 words** on backgrounds wider than one column:
   each column was upscaled alone, so the model invented different edges
   on both sides of the join. Adjacent columns of one reading are joined
   for the model and cut apart after.

9. **Joining bled a mask into a background**: some story records hold an
   unrelated white mask in the column next to the picture. Columns are
   joined only when the picture runs on across the join (the step across
   it is no bigger than the steps inside).

10. **Lines at every join of a box, the field's tiles, the text** ("rebarba"):
    the game builds those from pieces cut out of one sheet; upscaled whole,
    each piece's edge blends with its neighbour *on the sheet*. `--cuts`
    upscales every piece a dump saw that stands out from its surroundings
    on its own and lays it back over the sheet.

11. **Upscayl hung forever** on a 26x5 piece and the outputs after it came
    out as noise. Pieces smaller than 32 pixels go to the model padded with
    their own mirror image and are cut back out.

12. **The cards in the hand stayed low resolution** while the full-screen
    card was replaced. The hand draws the 40x32 thumbnails, which the duel
    copies from the streamed sectors into a table before uploading, so the
    upload's bytes were at no delivered address and had no provenance. An
    upload no delivery covers is now searched for by content in the ring of
    delivered sectors (`found_by_content` in `texture_dump.c`).

13. **Some entries matched nothing** after sorting: entries at the same
    offset with different geometry could interleave the readings of one
    sheet. Entries sort by offset, geometry, depth and palette.

14. **Upscayl CLI details**: `-r` (resize) is ignored, so the tool resizes
    with Pillow; its messages are UTF-8 with emoji (decode as bytes); it
    names outputs after the inputs; a run can take 1–4 GB of memory, and a
    background run was once killed by the session for memory pressure.
    Identical pictures (the duel-hand block is in all seven terrains and
    the Library) are one file and one upscale.

15. **Software 4x with a full pack is too slow for scripted checks** (a
    duel case passed 10 minutes). Use 2x for software oracles.

16. **The build copies `mods/*` into `tmp/pc/game32/mods`**: an old pack
    left in `mods/` reappears in the game's Mods window after every build.

17. **Black turned transparent at 1x.** The shadow marks a texel painted
    transparent as 0x8000, and a black pixel came out as the same 0x8000;
    the renderer took both for holes, so a pack that painted black showed
    what lay underneath. Opaque black is its own cell value
    (`TEXTURE_SHADOW_BLACK`, `texture_dump.h`). The 1:1 oracle still draws
    0 pixels off on the main menu and in a duel.

18. **A very wide image drew the whole GL picture black**: GL cannot make a
    texture past `GL_MAX_TEXTURE_SIZE`. Such an image is now averaged down
    to fit in `gl_picture.c` (`shrunk`), with a warning.

## Why it looked bad

The engine drew what the pack said; the pack was the problem. With
`high-fidelity-4x` on everything:

- UI art, fonts and icons are 4-bit, few colours, hard edges. The model
  turns them soft and painterly; text becomes blurry and loses its outline.
- Dithered and noisy backgrounds come out smeared, with invented texture.
- The story pictures and card arts fared better, but still look filtered
  rather than redrawn.

For a next attempt:

- Do not run fonts, glyph sheets and small UI through a photo model.
  Leave them original (`--only` / `--min-size`), or redraw them by hand
  and `--merge` them.
- Choose the model per family (an anime/illustration model for scenes and
  cards, a pixel-art-aware scaler such as xBRZ or ScaleFX for UI), or use
  hand-painted art for the few screens that matter.
- Check every screen, not just the first duel: the cuts and palettes of a
  screen are known only once it has been dumped.

## Reaching every screen without a player

Which palettes a screen uses and where it cuts its sheets are known from a
dump of that screen. To dump screens no scripted case reaches (Free Duel,
Password, Library, game over, the campaign's card shop, the map), the port
can run the game's own debug menu:

```sh
MEMORIES_MODE_AT=1000:0   # with the options smoke case's input
```

`MEMORIES_MODE_AT=<frame>:<mode>[,<frame>:<mode>...]` replaces the next
main mode a screen publishes after that frame (a menu choice, a screen's
exit) with `<mode>` (`src/game/main_modes.h`; 0 is the debug menu). With
the options case's input, choosing OPTION on the main menu opens the debug
menu instead; checked. Its entries (`notes/debug-menu-entry-map.md`) enter
Campaign (with a scene or message id), DUEL, BustUp, 3D MAP, DeckEdit,
FreeDUEL, NAME, Password, Load, Save, Trade, Option. Driving it with
`MEMORIES_INPUT` (d-pad moves the cursor, two columns of ten) and
`MEMORIES_DUMP_TEXTURES` gives each screen's `assets.txt`; that part has
not been scripted yet.

A static alternative exists for the sprites drawn from a screen's display
resource bank (the one-sector phase every package loads to `0x801AF000`, or
`0x801AF800` for the main menu): a three-level tree of little-endian u16
offsets (`DisplayObject_UpdateCommandStream`) leads to a command stream of
`(duration, u16 frame offset)` entries with opcodes `0xF9`–`0xFF`
(`DisplayObjectStream_ReadNextCommand`, `display_object_runtime.c`), and
each frame is a 4-byte header (count, flags, page, palette step) followed
by 6-byte parts: `u = (cell & 0x1F) * 8`, `v = ((cell >> 5) & 0x1F) * 8`,
width `((size >> 5) & 0xF) * 8 + 8`, height `((size >> 9) & 0xF) * 8 + 8`
(`DisplayObject_RenderSpriteSheet`). The base page and palette come from
the code that creates each object, and the duel's HUD draws some pieces
with constants in C rather than from a bank, so dumps remain the complete
source.

## Second attempt: one screen at a time, faithful (September 2026)

The Build Deck screen was redone with what the first attempt taught, and
the card assets were mapped from a set of redrawn HD images. Two tools:

- `tools/pc/hd_screen_pack.py <recipe>` enlarges a screen's sheet readings
  from the player's disc by a recipe (`tools/pc/hd_recipes/build_deck.json`):
  painted art through Real-ESRGAN (`realesrgan-x4plus`, 60 % mixed with a
  Lanczos enlargement so the model's invented grain goes), few-colour UI art
  through xBR (ffmpeg's `xbr` filter), a repeated tile wrapped at its edges.
  Every result is back-projected so each 4x4 block averages to its texel:
  colours and shading stay the game's. A recipe lists only the pieces a
  capture saw drawn; the rest of a reading stays the texels, four times.
- `tools/pc/hd_assets_pack.py --assets <folder>` places redrawn assets
  (card art, thumbnails, frames, back, attribute balls, level star, digits and
  labels) where the game keeps them, in every package and palette that
  draws them, over a `hd_screen_pack.py` pack (`--base`), and merges other
  packs (`--merge`) into one mod.

What was found on the way:

1. **Build Deck's package was never extracted.** `extract_images.py` listed
   func_80032184's package (WA sector 0x2189) at 0x1112800; it is at
   0x10C4800, with its palette block at 0x10E8800. The same loader serves
   Trade (Main_RunTrade).
2. **The card-frame sheet is in ten packages**: Build Deck, Library,
   Password and the seven duel terrains carry the same words for its two
   columns, the back's foot and the fourth column's pieces, each package
   with its own palette block whose rows 8-15 are alike. Row 8 is the
   monster frame, 9 magic, 10 trap, 11 ritual, 12 and 13 purple and orange;
   the card back reads the same through every row.
3. **Per-region mirroring invents patterns.** A 7-texel stone strip
   enlarged alone with mirrored padding came out as a lattice of X-shaped
   cracks. A painted reading goes to the model whole (every piece with its
   real neighbours), and a piece the game cuts out on its own is redone alone
   with edge padding (`cut_stands_out`).
4. **A smoothed outline grows into the neighbours.** xBR on the alpha mask
   rounds icons nicely but pushes box corners past their texels; only
   regions up to 40 texels get the smoothed outline.
5. **The card names and labels are subtracted.** Their palette entries 1-7
   carry the semi-transparency bit and the card view draws them with the
   subtracting blend. A pack pixel keeps the original texel's bit, so an HD
   letter in another font came out half invisible (black subtracted is
   nothing) and half solid. Over those texels the letter is now the grey that
   subtracts to dark, as deep as it covers the pixel.
6. **Thumbnails are hand-framed crops.** Each card's 40x32 thumbnail is its
   art cut at its own rectangle; the rectangles were found by searching the
   game's art against the game's thumbnails (median error 10.7 of 255) and
   are kept in `tools/pc/hd_recipes/thumb_crops.json`.
7. **Memory.** A pack image is decoded on first use and kept. Card art is
   stored at exactly 4x (408x384) rather than larger.
8. **Small lettering cannot be scaled clean.** xBR turned CHEST, ORDER and
   the outlined digits into wobbly, bloated letters (their 1-texel
   anti-aliasing and shadow read as shapes), and rebuilding each colour's
   region as a smooth shape came out ragged. The recipe's `labels` set them
   anew in the bold sans HD text uses, fitted to the game's letters, through
   every palette the game reads them with: the digits through six (white,
   yellow, blue, green, grey, red). The cursor row's digits stay the game's:
   they pulse through a palette the game dims at run time, which a pack
   cannot match (it matches palettes by where they are on the disc).
9. **One mod, in parts.** `hd_assets_pack.py` gives each entry a `setting`
   (card art, thumbnails, card frames, Build Deck screen, and each merged
   pack), and the mod declares them as bool settings, so the Mods window
   switches each part on and off at once (`notes/modding.md`).

## Third: the duel (September 2026)

`tools/pc/hd_recipes/duel.json` does the duel the same way; it is the
Forbidden Memories HD mod's "duel" part (`hd_assets_pack.py --base` takes
it beside Build Deck's). It was captured with the campaign duel of
`tests/pc/smoke/duel-hand-camera.json` and `MEMORIES_DUMP_TEXTURES_FROM`
set once the duel began, then every field was drawn by setting
`gDuel_bTerrain` (0x8009B364) from a throwaway change on the PC side.

1. **The platform is phase 12 of each terrain's package**: the floor is its
   first column (five bands of five 51x51 tiles, band k through the
   package's palette row at +0x7F00 + 0x20k), the sides and trim its second
   (ten palettes from +0xF120). One code draws every field, so the normal
   field's pieces are every field's at each package's offsets. The tiles are
   enlarged one by one (`alone`), so no tile's edge takes its neighbour's.
2. **The hand's card frames** are an 8-bit reading of phase 0's third
   column, palette rows 1-6 (monster, magic, trap, ritual, purple, orange;
   `extract_images.py` knew rows 1 and 2 only). The same sheet holds the
   face-down back, the card-kind words, two sets of small digits and a large
   one, and the sword and shield (`pixel`: xBR on a painted reading).
   Phase 0 is the same words in all seven packages, and a reading of the
   same pixels is made once.
3. **The same digits are drawn plain and subtracted.** The hand draws its
   numbers and card-kind words with the subtracting blend (the entries carry
   the semi-transparency bit), the life points draw the same texels plain,
   and a pack pixel over a clear texel is never blended. A letter reaching
   past the game's own therefore showed dark squares in one place or light
   letters in the other; the recipe's labels are `clip`ped to the game's
   glyph texels, which reads right in both.
4. **The FIELD box and the life points** are in the resident UI package
   (0xB50000, palettes 0xB609A0 and 0xB609C0). The box's middle is one
   piece repeated, done alone with its edges carried on so the repeats meet
   without a seam. The field names are anti-aliased words cut into pieces
   drawn side by side, some through a letter (MEAD + OW, DA + RK): a
   `strip` label sets the word once across them and cuts it back.
5. **Cropping a reading to its rows broke the life points.** The pack gives
   each VRAM word one entry's geometry, whatever palette draws it, so two
   readings of one sheet cropped differently took each other's words. Every
   entry keeps the whole sheet; `trim` instead leaves what a reading never
   draws as its texels four times, and the duel's images are stored as 256
   colours with transparency. The part adds 13 MB to the mod.
6. The upscaler has twice left a truncated output on a full tmpfs `/tmp`;
   the tool checks every output and runs again (`TMPDIR` on a disk helps).

## Not done

- The campaign map's own pictures (uploaded by the overworld overlay from
  its 134-sector block).
- `MODEL.MRG`: the 3D monsters' textures.
- An 8-bit sprite can span two 64-word columns; no dump showed one, and on
  a screen that does it would show half replaced.
- Freeing pack images and GL textures that are no longer on screen.
