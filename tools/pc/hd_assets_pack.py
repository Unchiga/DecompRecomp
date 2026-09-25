#!/usr/bin/env python3
"""Turn a folder of redrawn HD card assets into a texture pack.

The assets are drawn to their own layout, not the game's; this tool knows
where each goes on the disc and in which palettes the game draws it:

Card names are not taken from the assets: the port draws every card's plate
from its name (src/pc/cards/art.c), the base cards' and the ones mods add
alike, so they stay one style.

  cards/<n>.png        card n's art (102x96 texels), and its 40x32
                       thumbnail (the duel hand, sector n-1), cut from the
                       art where the game's own thumbnail is cut from the
                       game's art (--thumb-crops, found once by search)
  frame_monster.png, frame_magic.png, frame_trap.png, frame_ritual.png
                       the card-frame sheet (two 8-bit columns, 256x256
                       texels at 4x), one per palette row 8-11; rows 12
                       and 13 (purple, orange) are the monster frame
                       recoloured as the game's rows recolour it
  backcard/Back.png    the card back: 128x128 texels at the foot of the
                       frame sheet's second column and 128x64 at the head
                       of the package's third, drawn through every row
  attributes/<a>.png   the attribute balls, 16x16 texels at (16k, 128) of
                       the package's fourth column, ball k through palette
                       row 15 entry 16k: light, dark, earth, water, fire,
                       wind, magic, trap
  levels/star.png      the level star, 9x9 at (0, 144) of that column
  stats/stats.png      the digits, the three [... Card] labels, ATK and
                       DFD: split into its groups, each fitted to the
                       pixels of the game's own

The frame sheet is the same words in ten packages (Build Deck, Library,
Password and the seven duel terrains), and so are the back's foot and the
fourth column's pieces; each package has its own palette block at (256,
240) whose rows 8-15 are the same. Every copy gets the entries.

The labels are drawn subtracted from the frame (their palette entries 1-7
carry the semi-transparency bit, which a pack pixel keeps from the original
texel): over those texels a letter is written as the grey that subtracts to
dark (blend_ready), elsewhere as it is.

--base takes an existing pack (hd_screen_pack.py's output; repeat it for
several): a reading it has starts from its image, the assets drawn over it,
and its other entries join the part its recipe names. --merge adds another
pack's entries as they are (a portraits mod). The result is one mod, in
parts the player can switch off in the Mods window (PARTS; each merged pack
is a part of its own, named as that mod is).

Usage: hd_assets_pack.py --assets <folder> --out <mod folder> [--data game/DATA]
                         [--base <pack> ...] [--merge <pack> ...] [--thumb-crops crops.json]
                         [--id forbidden-memories-hd] [--name "Forbidden Memories HD"]
"""
import argparse
import json
import os
import shutil
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_images as X  # noqa: E402

S = 4
SECTOR = 2048
WA = "WA_MRG.MRG"
# (name, image phase at VRAM (768, 256), palette block at (256, 240))
PACKAGES = [("deck", 0x10C4800, 0x10E8800), ("library", 0xEE6800, 0xF06800), ("password", 0xF97800, 0xFB7800)] + [
    (f"duel-{name}", 0xB63000 + i * 0xEB * SECTOR, 0xB63000 + i * 0xEB * SECTOR + 64 * SECTOR)
    for i, name in enumerate(("normal", "forest", "wasteland", "mountain", "meadow", "sea", "dark"))]
DECK_BLOCK = 0x10E8800
# The mod's parts: its settings, and each manifest entry's "setting".
PARTS = {
    "card_art": ("Card art", "The redrawn pictures on the cards."),
    "thumbnails": ("Card thumbnails", "The small pictures in the duel hand and on the field."),
    "card_frames": ("Card frames", "Frames, card back, attribute balls, level stars and the card view's digits and labels."),
    "build_deck": ("Build Deck screen", "The Build Deck and Trade screen's panels, icons and labels."),
    "duel": ("Duel arena and HUD", "The platform of all seven fields, the cards' frames in the hand, their labels "
             "and numbers, the FIELD box and the life points."),
}
FRAMES = {8: "frame_monster.png", 9: "frame_magic.png", 10: "frame_trap.png", 11: "frame_ritual.png"}
ATTRIBUTES = ("light", "dark", "earth", "water", "fire", "wind", "magic", "trap")
# The game's ball has a one-texel rim it subtracts from the name bar (a
# shade); an HD ball of another colour cannot be made by subtracting, so it
# sits inside the rim (the largest circle clear of it) and the rim is left out.
BALL_DIAMETER = 12.8
STAR_PALETTE, LABEL_PALETTE = 0x1180, 0x11E0
DIGITS = [(16 + 6 * i, 144, 6, 13) for i in range(10)]
LABELS = [(0, 158, 56, 16), (0, 174, 56, 16), (0, 190, 56, 16), (0, 206, 24, 12), (0, 218, 24, 12)]


def card_base(n):
    return ((n - 1) * 7 + 722) * SECTOR


class Pack:
    def __init__(self, data, out):
        self.wa = open(os.path.join(data, WA), "rb").read()
        self.out, self.entries, self.images = out, [], {}
        self.part, self.parts = None, {}   # the part entries are added to now, and every part's (label, help)
        os.makedirs(os.path.join(out, "textures"), exist_ok=True)

    def original(self, offset, words, rows, bpp, clut, entries):
        palette = X.read_palette(self.wa, clut, entries) if bpp != 16 else None
        w, h, px = X.decode(self.wa, offset, words, rows, bpp, palette)
        return Image.frombytes("RGBA", (w, h), px)

    def add(self, name, image, offset, words, rows, bpp, clut, entries, alias, paletted=False):
        """One entry; the same pixels already written are the same file.
        `paletted`: an opaque picture kept as 256 colours of its own
        (libimagequant, dithered), as the game keeps its card art; about a
        third of the size, and decoded to the same pixels' worth in game;
        "alpha" keeps the transparency (the duel's sheets)."""
        key = image.tobytes()
        if key not in self.images:
            self.images[key] = name
            if paletted == "alpha":
                image = image.convert("RGBA").quantize(256, method=Image.Quantize.LIBIMAGEQUANT,
                                                       dither=Image.Dither.FLOYDSTEINBERG)
            elif paletted:
                image = image.convert("RGB").quantize(256, method=Image.Quantize.LIBIMAGEQUANT,
                                                      dither=Image.Dither.FLOYDSTEINBERG)
            image.save(os.path.join(self.out, "textures", name), optimize=True)
        self.entries.append({"file": self.images[key], "alias": alias, "archive": WA, "offset": offset,
                             "words": words, "rows": rows, "bpp": bpp, "width": words * {4: 4, 8: 2}[bpp],
                             "height": rows, "crop_left": 0, "clut_offset": clut, "clut_entries": entries,
                             "stride": words, "row_offsets": None, "setting": self.part})
        self.parts.setdefault(self.part, PARTS.get(self.part))


def load(path, size=None):
    image = Image.open(path).convert("RGBA")
    return image.resize(size, Image.LANCZOS) if size and image.size != size else image


def column_base(pack, bases, offset, bpp, clut):
    """A sheet column at 4x: the base pack's image of that reading, or the
    original's texels four times."""
    path = bases.get((offset, bpp, clut))
    if path:
        return Image.open(path).convert("RGBA")
    entries = 256 if bpp == 8 else 16
    image = pack.original(offset, 64, 256, bpp, clut, entries)
    return image.resize((image.width * S, image.height * S), Image.NEAREST)


def recolour(hd, original_row, original_ref):
    """The monster frame in another row's colours: each pixel times the
    game's ratio of the two rows there (smoothed to the HD size)."""
    a = np.array(original_row.convert("RGB").resize(hd.size, Image.BILINEAR), dtype=np.float64)
    b = np.array(original_ref.convert("RGB").resize(hd.size, Image.BILINEAR), dtype=np.float64)
    rgb = np.array(hd, dtype=np.float64)
    rgb[..., :3] = np.clip(rgb[..., :3] * (a + 8) / (b + 8), 0, 255)
    return Image.fromarray(rgb.round().astype(np.uint8))


def semi_texels(wa, offset, words, rows, clut, stride=None):
    """Which texels of a 4-bit image the game blends (their palette entry has
    the semi-transparency bit): the pack's colour there is what the blend
    uses, and the bit is always the original's (texture_pack.c)."""
    palette = X.read_palette(wa, clut, 16)
    stride = stride or words
    out = np.zeros((rows, words * 4), bool)
    for y in range(rows):
        for x in range(words * 4):
            word = wa[offset + y * stride * 2 + (x // 4) * 2] | wa[offset + y * stride * 2 + (x // 4) * 2 + 1] << 8
            colour = palette[(word >> ((x % 4) * 4)) & 15]
            out[y, x] = colour != 0 and colour & 0x8000
    return out


# The dark a letter subtracted from the card makes (the frame's paper less
# the grey below), for the parts of an HD letter the game draws plain.
INK = (24, 12, 4)


def blend_ready(image, semi, rect, text, original=None):
    """Make an HD piece right for the texels the game draws blended. The game
    subtracts them (the labels' dark letters): a letter there
    becomes a grey the blend turns dark, as deep as the letter covers the
    pixel, opaque so the blend always runs; where the HD letter reaches past
    the game's texels it is drawn plain, so there it is that dark, as
    transparent as it leaves the pixel (the picture mixes it over the card).
    How much a pixel is letter comes from its alpha and its darkness, so the
    letters' smoothed edges and shading stay smooth. Anything else keeps the
    game's own colour on those texels (a star's or ball's rim)."""
    x0, y0, w, h = rect
    a = np.array(image)
    for ty in range(y0, y0 + h):
        for tx in range(x0, x0 + w):
            block = a[ty * S:(ty + 1) * S, tx * S:(tx + 1) * S]
            if text:
                light = block[..., :3].astype(np.float64).mean(axis=2, keepdims=True)
                cover = block[..., 3:4].astype(np.float64) / 255 * np.clip((255 - light) / 110, 0, 1)
                if semi[ty, tx]:
                    block[..., :3] = (189 * cover).round().astype(np.uint8)
                    block[..., 3] = 255
                else:
                    block[..., :3] = INK
                    block[..., 3:4] = (255 * cover).round().astype(np.uint8)
            elif semi[ty, tx]:
                block[...] = np.array(original.getpixel((tx, ty)), np.uint8)
    return Image.fromarray(a)


def drop_blended(image, semi, rect):
    """Leave out the texels the game blends: transparent in the pack, so the
    primitive draws nothing there (texture_pack.c: alpha below half)."""
    x0, y0, w, h = rect
    a = np.array(image)
    for ty in range(y0, y0 + h):
        for tx in range(x0, x0 + w):
            if semi[ty, tx]:
                a[ty * S:(ty + 1) * S, tx * S:(tx + 1) * S] = 0
    return Image.fromarray(a)


# The inside of the frame's description boxes (texels of the sheet's first
# column): the monster box and the wide box of magic, trap and ritual cards.
# The game prints the same unreadable lines in them on every card.
BOXES = [(21, 159, 42, 22), (21, 223, 99, 22)]


def scribble(hd, original):
    """The game's lines in the description boxes, over the HD frame: how much
    each texel darkens the paper around it in the original (its brightness
    over the paper's, smoothed), enlarged and multiplied in."""
    from PIL import ImageFilter
    grey = original.convert("L")
    out = np.array(hd).astype(np.float64)
    for x, y, w, h in BOXES:
        box = grey.crop((x - 2, y - 2, x + w + 2, y + h + 2))
        paper = box.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(1.5))
        ratio = np.clip(np.array(box, np.float64) / np.maximum(np.array(paper, np.float64), 1), 0.35, 1.0)
        big = Image.fromarray((ratio * 255).round().astype(np.uint8)).resize(((w + 4) * S, (h + 4) * S), Image.LANCZOS)
        big = np.array(big, np.float64)[2 * S:-2 * S, 2 * S:-2 * S] / 255
        region = out[y * S:(y + h) * S, x * S:(x + w) * S]
        region[..., :3] *= np.clip(big, 0, 1)[..., None]
    return Image.fromarray(np.clip(out, 0, 255).round().astype(np.uint8))


def over(base, piece, x, y):
    base.alpha_composite(piece, (x, y))


def clear(image, rect):
    x, y, w, h = rect
    image.paste((0, 0, 0, 0), (x * S, y * S, (x + w) * S, (y + h) * S))


def opaque_box(image, rect=None):
    a = np.array(image.split()[-1])
    if rect:
        x, y, w, h = rect
        a = a[y:y + h, x:x + w]
    ys, xs = np.nonzero(a >= 128)
    if not len(ys):
        return None
    x0, y0 = (rect[0], rect[1]) if rect else (0, 0)
    return x0 + xs.min(), y0 + ys.min(), xs.max() - xs.min() + 1, ys.max() - ys.min() + 1


def groups(image, axis):
    """Runs of rows (axis 1) or columns (axis 0) holding opaque pixels."""
    on = (np.array(image.split()[-1]) >= 128).any(axis=axis)
    runs, start = [], None
    for i, v in enumerate(list(on) + [False]):
        if v and start is None:
            start = i
        elif not v and start is not None:
            runs.append((start, i))
            start = None
    return runs


def fit(sheet, piece, box):
    """The piece (trimmed to its pixels) stretched onto the game's pixels' box."""
    trimmed = piece.crop(piece.getbbox())
    x, y, w, h = box
    over(sheet, trimmed.resize((w * S, h * S), Image.LANCZOS), x * S, y * S)


def stats_pieces(stats):
    """stats.png's groups top to bottom: the digits (one image each), then the
    labels [Normal Magic Card], [Trap Card], [Equip Magic Card], ATK, DFD."""
    bands = [stats.crop((0, a, stats.width, b)) for a, b in groups(stats, 1)]
    if len(bands) != 6:
        sys.exit(f"stats.png: expected 6 rows of pieces, found {len(bands)}")
    runs = groups(bands[0], 0)
    width = sorted(b - a for a, b in runs)[len(runs) // 2]
    split = []
    for a, b in runs:   # two digits that touch: halved
        split += [(a, (a + b) // 2), ((a + b) // 2, b)] if b - a > width * 1.6 else [(a, b)]
    digits = [bands[0].crop((a, 0, b, bands[0].height)) for a, b in split]
    if len(digits) != 10:
        sys.exit(f"stats.png: expected 10 digits, found {len(digits)}")
    return digits, bands[1:]


def build(args):
    pack = Pack(args.data, args.out)
    A = args.assets
    bases = {}
    replaced = set()   # the base readings the assets draw over, added with them below
    for base in args.base or []:
        with open(os.path.join(base, "textures", "manifest.json"), encoding="utf-8") as handle:
            for entry in json.load(handle):
                bases[(entry["offset"], entry["bpp"], entry.get("clut_offset"))] = \
                    os.path.join(base, "textures", entry["file"])
    back = load(os.path.join(A, "backcard", "Back.png"), (128 * S, 192 * S))

    # The frame sheet and the card back, per package and row.
    pack.part = "card_frames"
    for name, phase, block in PACKAGES:
        ref = [pack.original(phase + c * 0x8000, 64, 256, 8, block + 8 * 0x200, 256) for c in (0, 1)]
        for row in range(8, 14):
            clut = block + row * 0x200
            hd = load(os.path.join(A, FRAMES.get(row, FRAMES[8])), (256 * S, 256 * S))
            if row not in FRAMES:
                both = Image.new("RGBA", (256, 256))
                for c in (0, 1):
                    both.paste(pack.original(phase + c * 0x8000, 64, 256, 8, clut, 256), (c * 128, 0))
                refboth = Image.new("RGBA", (256, 256))
                refboth.paste(ref[0], (0, 0))
                refboth.paste(ref[1], (128, 0))
                hd = recolour(hd, both, refboth)
            hd = scribble(hd, ref[0])
            for c in (0, 1):
                offset = phase + c * 0x8000
                sheet = column_base(pack, bases, offset, 8, clut)
                over(sheet, hd.crop((c * 128 * S, 0, (c + 1) * 128 * S, 256 * S)), 0, 0)
                if c == 1:
                    sheet.paste(back.crop((0, 0, 128 * S, 128 * S)), (0, 128 * S))
                pack.add(f"frame-r{row}-c{c}.png", sheet, offset, 64, 256, 8, clut, 256,
                         f"card frame ({name}, row {row}), column {c}")
                replaced.add((offset, 8, clut))
    digits, labels = stats_pieces(load(os.path.join(A, "stats", "stats.png")))
    # The back's foot (third column) and the fourth column's pieces: the same
    # words at the same places in every package.
    for pname, phase, block in PACKAGES:
        c2, c3 = phase + 2 * 0x8000, phase + 3 * 0x8000
        for row in range(8, 14):
            clut = block + row * 0x200
            sheet = column_base(pack, bases, c2, 8, clut)
            sheet.paste(back.crop((0, 128 * S, 128 * S, 192 * S)), (0, 0))
            pack.add(f"back-{pname}-r{row}.png", sheet, c2, 64, 256, 8, clut, 256, f"card back foot ({pname}, row {row})")
            replaced.add((c2, 8, clut))
        for k, attribute in enumerate(ATTRIBUTES):
            clut = block + 0x1E00 + k * 0x20
            sheet = column_base(pack, bases, c3, 4, clut)
            # The HD ball sits inside the rim (BALL_DIAMETER); the rim's
            # blended texels are left out.
            clear(sheet, (16 * k, 128, 16, 16))
            ball = load(os.path.join(A, "attributes", attribute + ".png"))
            ball = ball.crop(ball.getbbox())
            size = round(BALL_DIAMETER * S)
            at = round((8 - BALL_DIAMETER / 2) * S)
            over(sheet, ball.resize((size, size), Image.LANCZOS), 16 * k * S + at, 128 * S + at)
            sheet = drop_blended(sheet, semi_texels(pack.wa, c3, 64, 256, clut), (16 * k, 128, 16, 16))
            pack.add(f"attribute-{pname}-{attribute}.png", sheet, c3, 64, 256, 4, clut, 16,
                     f"attribute ball: {attribute} ({pname})")
            replaced.add((c3, 4, clut))
        clut = block + STAR_PALETTE
        original = pack.original(c3, 64, 256, 4, clut, 16)
        sheet = column_base(pack, bases, c3, 4, clut)
        clear(sheet, (0, 144, 9, 9))
        fit(sheet, load(os.path.join(A, "levels", "star.png")), opaque_box(original, (0, 144, 9, 9)))
        sheet = blend_ready(sheet, semi_texels(pack.wa, c3, 64, 256, clut), (0, 144, 9, 9), False, original)
        pack.add(f"level-star-{pname}.png", sheet, c3, 64, 256, 4, clut, 16, f"level star ({pname})")
        clut = block + LABEL_PALETTE
        original = pack.original(c3, 64, 256, 4, clut, 16)
        sheet = column_base(pack, bases, c3, 4, clut)
        for piece, rect in list(zip(digits, DIGITS)) + list(zip(labels, LABELS)):
            box = opaque_box(original, rect)
            clear(sheet, rect)
            if box:
                fit(sheet, piece, box)
        semi = semi_texels(pack.wa, c3, 64, 256, clut)
        for rect in DIGITS + LABELS:
            sheet = blend_ready(sheet, semi, rect, True)
        pack.add(f"stats-{pname}.png", sheet, c3, 64, 256, 4, clut, 16, f"digits, card-kind labels, ATK, DFD ({pname})")
        replaced.update({(c3, 4, block + STAR_PALETTE), (c3, 4, block + LABEL_PALETTE)})

    # The rest of the base packs as they are, each entry in the part its recipe
    # names (a pack from before recipes had parts is the Build Deck's).
    for base in args.base or []:
        with open(os.path.join(base, "textures", "manifest.json"), encoding="utf-8") as handle:
            for entry in json.load(handle):
                if (entry["offset"], entry["bpp"], entry.get("clut_offset")) in replaced:
                    continue
                pack.part = entry.get("setting", "build_deck")
                image = Image.open(os.path.join(base, "textures", entry["file"])).convert("RGBA")
                pack.add("screen-" + entry["file"], image, entry["offset"], entry["words"], entry["rows"],
                         entry["bpp"], entry["clut_offset"], entry["clut_entries"], entry["alias"],
                         paletted="alpha" if pack.part == "duel" else False)

    # Card art and thumbnails.
    crops = {}
    if args.thumb_crops:
        with open(args.thumb_crops, encoding="utf-8") as handle:
            crops = {int(k): v for k, v in json.load(handle).items()}
    files = {}
    for folder in ("cards",):
        files[folder] = {int(os.path.splitext(f)[0]): os.path.join(A, folder, f)
                         for f in os.listdir(os.path.join(A, folder)) if os.path.splitext(f)[0].isdigit()}
    for n in range(1, 723):
        base = card_base(n)
        if n in files["cards"]:
            art = load(files["cards"][n])
            pack.part = "card_art"
            pack.add(f"card-{n:03d}.png", art.resize((102 * S, 96 * S), Image.LANCZOS), base, 0x33, 0x60, 8,
                     base + 0x2640, 256, f"card {n} art", paletted=True)
            if n in crops:
                _, x, y, w, h = crops[n]
                sx, sy = art.width / 102, art.height / 96
                thumb = art.crop((round(x * sx), round(y * sy), round((x + w) * sx), round((y + h) * sy)))
                small = (n - 1) * SECTOR
                pack.part = "thumbnails"
                pack.add(f"thumb-{n:03d}.png", thumb.resize((40 * S, 32 * S), Image.LANCZOS), small, 20, 32, 8,
                         small + 0x500, 64, f"card {n} thumbnail", paletted=True)

    # Other packs as they are.
    for other in args.merge or []:
        with open(os.path.join(other, "mod.json"), encoding="utf-8") as handle:
            merged = json.load(handle)
        folder = os.path.join(other, merged["textures"])
        part = merged.get("id") or os.path.basename(os.path.normpath(other))
        pack.parts[part] = (merged.get("name", part), merged.get("description", ""))
        with open(os.path.join(folder, "manifest.json"), encoding="utf-8") as handle:
            for entry in json.load(handle):
                target = os.path.join(pack.out, "textures", os.path.basename(folder) + "-" + entry["file"])
                shutil.copyfile(os.path.join(folder, entry["file"]), target)
                pack.entries.append(dict(entry, file=os.path.basename(target), setting=part))

    with open(os.path.join(pack.out, "textures", "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(pack.entries, handle, indent=1)
    return pack


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--assets", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--data", default="game/DATA")
    parser.add_argument("--base", action="append", help="a hd_screen_pack.py pack (repeat for several)")
    parser.add_argument("--merge", action="append")
    parser.add_argument("--thumb-crops")
    parser.add_argument("--id", default="forbidden-memories-hd")
    parser.add_argument("--name", default="Forbidden Memories HD")
    parser.add_argument("--author", default="Unchiga, X@nder")
    args = parser.parse_args()
    if os.path.isdir(os.path.join(args.out, "textures")):
        shutil.rmtree(os.path.join(args.out, "textures"))
    pack = build(args)
    manifest = {"id": args.id, "name": args.name, "version": "1.0", "author": args.author,
                "description": "HD card art, thumbnails, frames, card back, attribute balls, the "
                               "Build Deck screen and the duel (arena, hand, FIELD box, life points); the Free "
                               "Duel portraits. Shows best at View > Internal 4x with HD text on.",
                "enabled": True, "textures": "textures",
                "settings": [{"key": key, "label": label, "type": "bool", "default": 1, "description": help}
                             for key, (label, help) in sorted(pack.parts.items(), key=lambda part: (
                                 list(PARTS).index(part[0]) if part[0] in PARTS else len(PARTS)))]}
    with open(os.path.join(args.out, "mod.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=4)
    print(f"{args.out}: {len(pack.entries)} entries, {len(pack.images)} images")


if __name__ == "__main__":
    main()
