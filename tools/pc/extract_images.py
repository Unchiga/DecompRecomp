#!/usr/bin/env python3
"""Extract the game's images from the disc archives as PNG, named by origin.

The archives hold no image files: each screen's loader streams raw VRAM
words from a known sector and uploads them as rectangles, with the palette
in another rectangle (notes/mrg-files.md). So an image is defined by its
loader, and this tool replays the loaders it knows, family by family:

  cards      the 722 cards' artwork (func_800289BC: WA_MRG.MRG sector
             (n-1)*7 + 722, seven sectors; the 102x96 8-bit picture, its
             256-entry palette, the strip under it and the strip beside it)

  portraits  the 48x48 8-bit dialogue portraits with their 64-entry
             palettes, 0x980 bytes a record: the campaign's 25 (WA offset
             0xF35000, notes/dialog-portrait-bank.md) and Free Duel's 40
             (0xF55000, notes/mrg-files.md)

  sheets     every screen package whose loader streams its images through
             the GPU path (file_transfer_runtime.c): the main menu (SU.MRG),
             the boot UI, the story dialogue UI, the campaign, Free Duel,
             name entry, password, options, game over, the duel results and
             rewards, the Library, the seven duel terrains and the campaign
             map's own strip. Each sector of such a phase is a 64x16-word
             tile; sixteen of them stack into a 64-word column, contiguous
             in the archive (0x8000 bytes), the next column 64 words to the
             right. One PNG per column and per way the game reads it (depth
             and palette): the readings come from the draw code and from
             dumps of the game drawing (--variants).

  scenes     the campaign's story pictures (ScriptImage_RequestTransfer):
             records of 33, 81 and 113 sectors from WA sector 0x21D5, the
             last sector the palette, the others 2, 5 or 7 columns.

Every PNG is listed in <out>/manifest.json with its provenance: archive,
byte offset, size in VRAM words and rows, depth, stride, and the palette's
offset, which is the identity a texture pack goes by. Names are aliases on
top. Two entries whose pixels come out identical (the duel-hand block is in
all seven terrains and the Library) share one PNG.

Usage: extract_images.py [--data game/DATA] [--out tmp/pc/images]
                         [--names cards.tsv] [--variants assets.txt ...]
                         [--assets assets.txt] [family ...]
cards.tsv (optional, columns card_number and name) adds the card's name to
its file name. --variants takes the assets.txt of a MEMORIES_DUMP_TEXTURES
run and adds every depth and palette the game used on a sheet to that
sheet's PNGs (the built-in list is what dumps have shown so far). --assets
extracts what such a run drew, as the run cut it, for what no family covers.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import struct
import sys
import zlib

SECTOR = 2048
COLUMN = 16 * SECTOR  # 16 tiles of 64x16 words: one 64-word column of 256 rows
WA = "WA_MRG.MRG"
SU = "SU.MRG"


def write_png(path: str, width: int, height: int, rgba: bytes) -> None:
    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    stride = width * 4
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride] for y in range(height))
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        handle.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        handle.write(chunk(b"IEND", b""))


def expand(colour: int) -> bytes:
    """A 15-bit VRAM word as RGBA; 0 is the transparent colour."""
    if colour == 0:
        return b"\x00\x00\x00\x00"
    # As the picture expands a word (soft_gpu.c, gl_picture.c): a pack made
    # of these PNGs as they are draws exactly what the disc's words draw.
    r, g, b = colour & 0x1F, (colour >> 5) & 0x1F, (colour >> 10) & 0x1F
    return bytes((r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255))


def decode(data: bytes, offset: int, words: int, rows: int, bpp: int, palette: list[int] | None,
           stride: int | None = None, row_offsets: list[int] | None = None) -> tuple[int, int, bytes]:
    """A rectangle of VRAM words as RGBA pixels; bpp 4/8 index the palette.
    stride: words from one row to the next in the archive (default: words);
    row_offsets: each row's byte offset from `offset` instead, when the rows
    are not evenly spaced."""
    per_word = {4: 4, 8: 2, 16: 1}[bpp]
    width = words * per_word
    stride = words if stride is None else stride
    out = bytearray()
    for y in range(rows):
        at = offset + (row_offsets[y] if row_offsets else y * stride * 2)
        row = data[at:at + words * 2]
        for x in range(words):
            word = row[x * 2] | (row[x * 2 + 1] << 8)
            if bpp == 16:
                out += expand(word)
            elif bpp == 8:
                out += expand(palette[word & 0xFF]) + expand(palette[word >> 8])
            else:
                for shift in (0, 4, 8, 12):
                    out += expand(palette[(word >> shift) & 0xF])
    return width, rows, bytes(out)


def read_palette(data: bytes, offset: int, entries: int) -> list[int]:
    return list(struct.unpack_from(f"<{entries}H", data, offset))


class Sheet:
    """An image phase streamed through the GPU path: `sectors` tiles from
    `base` in `archive`, the first at VRAM (x, y). `variants` are the
    (bpp, palette offset) pairs the game reads it with; `palette` is the
    package's palette block on the disc, `palette_rect` where it goes in
    VRAM (x, y, w, h), which turns a CLUT coordinate a dump saw into an
    offset in the block."""

    def __init__(self, archive: str, base: int, sectors: int, x: int, y: int, stem: str, alias: str,
                 variants: list[tuple[int, int]], group: str | None = None) -> None:
        self.archive, self.base, self.sectors, self.x, self.y = archive, base, sectors, x, y
        self.stem, self.alias, self.group = stem, alias, group
        self.variants: list[tuple[int, int, int | None]] = []  # (bpp, clut, column or None for every column)
        for variant in variants:
            self.add(*variant)

    def add(self, bpp: int, clut: int, column: int | None = None) -> bool:
        """True when new: not there for that column nor for every column."""
        if (bpp, clut, column) in self.variants or (bpp, clut, None) in self.variants:
            return False
        self.variants.append((bpp, clut, column))
        return True

    def covers(self, archive: str, offset: int) -> bool:
        return archive == self.archive and self.base <= offset < self.base + self.sectors * SECTOR

    def column_variants(self, column: int) -> list[tuple[int, int]]:
        return [(bpp, clut) for bpp, clut, at in self.variants if at is None or at == column]


class Extractor:
    def __init__(self, data_dir: str, out: str) -> None:
        self.data_dir = data_dir
        self.out = out
        self.manifest: list[dict] = []
        self.archives: dict[str, bytes] = {}
        self.sheets: list[Sheet] = []
        self.identical: dict[bytes, str] = {}  # pixels' digest -> the PNG already written
        self.emitted: set[tuple] = set()

    def archive(self, name: str) -> bytes:
        if name not in self.archives:
            with open(os.path.join(self.data_dir, name), "rb") as handle:
                self.archives[name] = handle.read()
        return self.archives[name]

    def image(self, archive: str, offset: int, words: int, rows: int, bpp: int,
              clut_offset: int | None, path: str, alias: str, clut_entries: int | None = None,
              stride: int | None = None, row_offsets: list[int] | None = None,
              crop: tuple[int, int] | None = None) -> None:
        data = self.archive(archive)
        entries = clut_entries if clut_entries is not None else {4: 16, 8: 256, 16: 0}[bpp]
        identity = (archive, offset, words, rows, bpp, clut_offset if entries else None, stride, crop)
        if identity in self.emitted:
            return  # the same reading of the same words, asked twice
        self.emitted.add(identity)
        palette = read_palette(data, clut_offset, entries) if entries else None
        if palette is not None and len(palette) < 256:
            palette = palette + [0] * (256 - len(palette))  # indices past a short palette
        width, height, rgba = decode(data, offset, words, rows, bpp, palette, stride, row_offsets)
        if crop and (crop[0] or crop[1] != width):
            left, span = crop
            rgba = b"".join(rgba[(y * width + left) * 4:(y * width + left + span) * 4] for y in range(height))
            width = span
        digest = hashlib.sha256(struct.pack("<II", width, height) + rgba).digest()
        path = path.replace(os.sep, "/")
        if digest in self.identical:
            path = self.identical[digest]  # the same picture again (a copy in another package): one file
        else:
            full = os.path.join(self.out, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            write_png(full, width, height, rgba)
            self.identical[digest] = path
        self.manifest.append({
            "file": path, "alias": alias, "archive": archive, "offset": offset,
            "words": words, "rows": rows, "bpp": bpp, "width": width, "height": height,
            "clut_offset": clut_offset, "clut_entries": entries, "stride": words if stride is None else stride,
            "row_offsets": row_offsets,
        })

    def assets(self, listing: str) -> None:
        """assets.txt from a MEMORIES_DUMP_TEXTURES run: every texture the game
        drew whose words and palette were traced to the disc, skipping what a
        family's sheet covers (the sheet has it whole)."""
        archives: list[tuple[str, int, int]] = []
        with open(listing, encoding="utf-8") as handle:
            for line in handle:
                fields = line.split()
                if not fields:
                    continue
                values = dict(field.split("=", 1) for field in fields[1:] if "=" in field)
                if fields[0] == "archive":
                    name = fields[1].split(";")[0]
                    archives.append((name, int(values["lba"]) * SECTOR, int(values["bytes"])))
                elif fields[0] == "asset":
                    offset, clut = int(values["offset"]), int(values["clut"])
                    entries, bpp = int(values["entries"]), int(values["bpp"])
                    home = [a for a in archives if a[1] <= offset < a[1] + a[2]]
                    if not home or (entries and not any(a[1] <= clut < a[1] + a[2] for a in home)):
                        continue  # not in one archive (a mod's data, or the executable's)
                    name, start, _ = home[0]
                    if any(sheet.covers(name, offset - start) for sheet in self.sheets):
                        continue
                    stem = f"{name.split('.')[0].lower()}-{offset - start:08x}-{values['words']}x{values['rows']}-{bpp}"
                    if entries:
                        stem += f"-p{clut - start:08x}"
                    rowofs = [int(v) for v in values["rowofs"].split(",")] if "rowofs" in values else None
                    crop = tuple(int(v) for v in values["px"].split(",")) if "px" in values else None
                    self.image(name, offset - start, int(values["words"]), int(values["rows"]), bpp,
                               clut - start if entries else None, f"assets/{stem}.png",
                               f"drawn as {values.get('png', '?')}", clut_entries=entries,
                               stride=int(values["stride"]) if "stride" in values else None, row_offsets=rowofs,
                               crop=crop)

    def variants(self, listing: str) -> int:
        """Every reading (depth, palette) a MEMORIES_DUMP_TEXTURES run made of
        a sheet's words joins that sheet's variants."""
        archives: list[tuple[str, int, int]] = []
        added = 0
        with open(listing, encoding="utf-8") as handle:
            for line in handle:
                fields = line.split()
                if not fields:
                    continue
                values = dict(field.split("=", 1) for field in fields[1:] if "=" in field)
                if fields[0] == "archive":
                    archives.append((fields[1].split(";")[0], int(values["lba"]) * SECTOR, int(values["bytes"])))
                elif fields[0] == "asset":
                    offset, clut, bpp = int(values["offset"]), int(values["clut"]), int(values["bpp"])
                    home = [a for a in archives if a[1] <= offset < a[1] + a[2]]
                    if not home or (clut and not any(a[1] <= clut < a[1] + a[2] for a in home)):
                        continue
                    name, start, _ = home[0]
                    for sheet in self.sheets:
                        if sheet.covers(name, offset - start):
                            column = (offset - start - sheet.base) // COLUMN
                            added += sheet.add(bpp, clut - start if clut else 0, column)
        return added

    def share_variants(self) -> int:
        """Records of one kind (the seven terrains, a mode's story pictures)
        are laid out alike: a reading a dump made of one record's column
        holds for that column of the others, its palette at the same place
        in each record."""
        added = 0
        groups: dict[str, list[Sheet]] = {}
        for sheet in self.sheets:
            if sheet.group:
                groups.setdefault(sheet.group, []).append(sheet)
        for members in groups.values():
            readings: set[tuple[int, int, int | None]] = set()
            for sheet in members:
                for bpp, clut, column in sheet.variants:
                    inside = sheet.base <= clut < sheet.base + sheet.sectors * SECTOR + 8 * SECTOR
                    readings.add((bpp, clut - sheet.base if inside else clut, column) if inside
                                 else (bpp, -clut - 1, column))  # absolute palettes (another package's) stay
            for sheet in members:
                for bpp, clut, column in readings:
                    added += sheet.add(bpp, clut + sheet.base if clut >= 0 else -clut - 1, column)
        return added

    def cards(self, names: dict[int, str]) -> None:
        """func_800289BC: seven sectors per card from WA sector 722."""
        for card in range(1, 723):
            base = ((card - 1) * 7 + 722) * SECTOR
            stem = f"{card:04d}"
            if card in names:
                stem += "-" + re.sub(r"[^a-z0-9]+", "-", names[card].lower()).strip("-")
            clut = base + 0x2640
            self.image(WA, base, 0x33, 0x60, 8, clut, f"cards/{stem}.png", f"card {card} art")
            self.image(WA, base + 0x2840, 0x18, 0x0E, 8, clut, f"cards/{stem}.strip.png",
                       f"card {card} strip below")
            self.image(WA, base + 0x2AE0, 0x08, 0x58, 8, clut, f"cards/{stem}.side.png",
                       f"card {card} strip beside")

    def portraits(self) -> None:
        """Campaign_LoadScenePackage and FreeDuel_Init: 0x980-byte records."""
        for bank, base, count in (("campaign", 0xF35000, 25), ("freeduel", 0xF55000, 40)):
            for index in range(count):
                record = base + index * 0x980
                self.image(WA, record, 24, 48, 8, record + 0x900, f"portraits/{bank}-{index:02d}.png",
                           f"{bank} portrait {index} (F6 id 0x{0x41 + index:02x})" if bank == "campaign"
                           else f"free duel portrait {index}", clut_entries=64)

    # The screen packages: (archive, image phase offset, sectors, VRAM x, y)
    # from each loader's stage callback (notes/mrg-files.md and the sources
    # named), and the readings dumps have shown: (bpp, palette offset).
    # A palette offset is the palette block's plus row * 0x200 plus, at
    # 4 bpp, the 16-colour CLUT's column * 0x20.
    def sheets_family(self) -> None:
        def sheet(archive: str, base: int, sectors: int, x: int, y: int, stem: str, alias: str,
                  variants: list[tuple[int, int]], group: str | None = None) -> None:
            self.sheets.append(Sheet(archive, base, sectors, x, y, stem, alias, variants, group))

        def rows8(block: int, *rows: int) -> list[tuple[int, int]]:
            return [(8, block + row * 0x200) for row in rows]

        def cluts4(block: int, *at: int) -> list[tuple[int, int]]:
            return [(4, block + offset) for offset in at]

        # MainMenu_LoadPackageStage: SU.MRG sectors 0-64 and 64-96, palette 256x8 at (0, 240) from +0x30000.
        sheet(SU, 0, 64, 0x200, 0x100, "sheets/menu/a", "main menu image A", rows8(0x30000, 0, 1, 2, 3))
        sheet(SU, 64 * SECTOR, 32, 0x380, 0, "sheets/menu/b", "main menu image B", cluts4(0x30000, 0x800))
        # Main_LoadBootPackageStage: WA 0xB48000, 48 sectors at (640, 0); palette 256x8 at (512, 248)
        # from 0xB60000 and the eight colour ramps 16x8 at (640, 232) from 0xB61000.
        sheet(WA, 0xB48000, 48, 0x280, 0, "sheets/boot/a", "boot UI image",
              cluts4(0xB60000, 0, 0x40, 0x80, 0x260, 0x2A0, 0x2E0, 0x300, 0x340, 0x460, 0x500, 0x520, 0x600, 0x620,
                     0x640, 0x660, 0x680, 0x6A0, 0x6C0, 0x6E0, 0x820, 0x840, 0x880, 0x9A0, 0x9C0, 0xE40)
              + cluts4(0xB61000, 0, 0x20, 0x40, 0x80, 0xA0))
        # Main_LoadBootImageStage (WA sector 0x1F85, 34): 32 sectors at (0, 256); palette 256x4 at (256, 240).
        sheet(WA, 0xFC2800, 32, 0, 0x100, "sheets/boot/b", "boot image (title)",
              cluts4(0xFD2800, 0, 0x20) + cluts4(0xB60000, 0xA00, 0xA20))
        # func_8002F4C0 (story dialogue UI, WA 0xFD3800): 48 sectors at (448, 256); palette 256x2 at (0, 244).
        sheet(WA, 0xFD3800, 48, 0x1C0, 0x100, "sheets/story/ui", "story dialogue UI",
              rows8(0xFEB800, 0) + cluts4(0xFEB800, 0x200))
        # Campaign_LoadScenePackageStage: 16 sectors at (832, 0); palette 256x1 at (256, 240) from 0xF33800.
        sheet(WA, 0xF2B800, 16, 0x340, 0, "sheets/campaign/ui", "campaign UI", cluts4(0xF33800, 0, 0x20))
        # FreeDuel_LoadPackageStage: 32 sectors at (0, 256); palette 256x4 at (0, 240) from 0xF54000.
        # The screens below have not been dumped yet: their readings are the first palette row at
        # 8 bpp and the first 16-colour palette at 4 bpp, a guess until a dump (--variants) says.
        sheet(WA, 0xF44000, 32, 0, 0x100, "sheets/freeduel/a", "free duel image",
              rows8(0xF54000, 0) + cluts4(0xF54000, 0))
        # NameEntry_LoadPackageStage: 48 sectors at (256, 256), 16 at (448, 256); palette 256x4 at (256, 240).
        sheet(WA, 0xF6F800, 48, 0x100, 0x100, "sheets/name/a", "name entry image A", rows8(0xF8F800, 3))
        sheet(WA, 0xF87800, 16, 0x1C0, 0x100, "sheets/name/b", "name entry image B",
              cluts4(0xF8F800, 0, 0x200, 0x220))
        # Password_LoadPackageStage: 64 sectors at (768, 256); palette 256x16 at (256, 240) from 0xFB7800.
        sheet(WA, 0xF97800, 64, 0x300, 0x100, "sheets/password/a", "password image",
              rows8(0xFB7800, 0, 8) + cluts4(0xFB7800, 0, 0x200))
        # Options_LoadPackageStage: 32 sectors at (0, 256); palette 256x4 at (256, 240) from 0x109A800.
        sheet(WA, 0x108A800, 32, 0, 0x100, "sheets/options/a", "options image",
              cluts4(0x109A800, 0, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0))
        # GameOver_LoadPackageStage: 48 sectors at (0, 256); palette 256x4 at (0, 240) from 0x10C3800.
        sheet(WA, 0x10AB800, 48, 0, 0x100, "sheets/gameover/a", "game over image",
              rows8(0x10C3800, 0) + cluts4(0x10C3800, 0))
        # func_80020BE4 (duel results): 32 sectors at (0, 256); palette 256x4 at (0, 248) from 0xEE5800.
        sheet(WA, 0xED5800, 32, 0, 0x100, "sheets/results/a", "duel results image",
              rows8(0xEE5800, 0) + cluts4(0xEE5800, 0))
        # func_80032184 (duel rewards, WA sector 0x2189): 64 sectors at (768, 256), 8 at (832, 0);
        # palette 256x16 at (256, 240) from 0x1136800.
        sheet(WA, 0x1112800, 64, 0x300, 0x100, "sheets/reward/a", "duel reward image A",
              rows8(0x1136800, 0) + cluts4(0x1136800, 0))
        sheet(WA, 0x1132800, 8, 0x340, 0, "sheets/reward/b", "duel reward image B",
              rows8(0x1136800, 0) + cluts4(0x1136800, 0))
        # func_8002BD0C (Library): 64 sectors at (768, 256), palette 256x16 at (256, 240) from 0xF06800;
        # 48 sectors at (576, 256), palette 256x2 at (256, 246) from 0xF20800.
        sheet(WA, 0xEE6800, 64, 0x300, 0x100, "sheets/library/a", "library image A",
              rows8(0xF06800, 0, 1, 2) + cluts4(0xF06800, 0))
        sheet(WA, 0xF08800, 48, 0x240, 0x100, "sheets/library/b", "library image B",
              rows8(0xF20800, 0) + cluts4(0xF20800, 0))
        # Duel_LoadPackageStage, one record of 235 sectors per terrain from WA sector 0x16C6: phase 0
        # 64 sectors at (768, 256) with phase 1's palette 256x16 at (256, 240); phase 6 32 sectors at
        # (512, 256) with phase 5's palette 256x8 at (0, 240); phase 10 8 sectors at (832, 0); phase 12
        # 32 sectors at (640, 256), whose 16-colour palettes are rows of the image itself (the game
        # reads them where they landed in VRAM).
        terrains = ("normal", "forest", "wasteland", "mountain", "meadow", "sea", "dark")
        for index, name in enumerate(terrains):
            base = 0xB63000 + index * 0xEB * SECTOR
            palette1, palette5 = base + 64 * SECTOR, base + 106 * SECTOR
            sheet(WA, base, 64, 0x300, 0x100, f"sheets/duel/{name}-p0", f"duel {name} image (phase 0)",
                  rows8(palette1, 1, 2) + cluts4(palette1, 0, 0x20, 0x60, 0x80, 0xA0, 0xC0), "duel-p0")
            sheet(WA, base + 108 * SECTOR, 32, 0x200, 0x100, f"sheets/duel/{name}-p6", f"duel {name} image (phase 6)",
                  rows8(palette5, 0), "duel-p6")
            sheet(WA, base + 190 * SECTOR, 8, 0x340, 0, f"sheets/duel/{name}-p10", f"duel {name} image (phase 10)",
                  rows8(palette5, 0), "duel-p10")
            sheet(WA, base + 203 * SECTOR, 32, 0x280, 0x100, f"sheets/duel/{name}-p12",
                  f"duel {name} image (phase 12)",
                  cluts4(base + 0x6D700, 0, 0x20, 0x40, 0x60, 0x80)
                  + cluts4(base + 0x74920, 0, 0x20, 0x40, 0x60, 0x80, 0xE0, 0x100, 0x120, 0x140, 0x160), "duel-p12")
        # CampaignMap_LoadPackageStage (both overworld variants): phase 3 16 sectors at (448, 256);
        # phase 4's palette 256x4 at (256, 240). The map's own pictures come from its 134-sector
        # block through the overworld overlay and are not here.
        for index, name in enumerate(("before", "after")):
            base = 0xFEC800 + index * 0x9E * SECTOR
            palette = base + 157 * SECTOR
            sheet(WA, base + 141 * SECTOR, 16, 0x1C0, 0x100, f"sheets/map/{name}", f"campaign map strip ({name})",
                  cluts4(palette, 0, 0x20, 0x40, 0x60), "map")
        # func_8003A01C (display_effect_resource_setup.c): 65 records of 50 sectors from WA sector 15182,
        # 48 sectors at (832 or 640, 256), a palette 256x2 at (512, 240 + 2k) from the 49th, a display
        # resource bank in the 50th. Drawn in the duel (a duelist's presentation).
        for index in range(65):
            base = (15182 + index * 50) * SECTOR
            sheet(WA, base, 48, 0x340, 0x100, f"sheets/effects/{index:02d}", f"display effect record {index}",
                  rows8(base + 48 * SECTOR, 0, 1), "effects")

    def scenes_family(self) -> None:
        """ScriptImage_RequestTransfer: the story's pictures, records from WA
        sector 0x21D5 of 33 (mode 0, 50 records), 81 (mode 1, 42) and 113
        sectors (mode 2); the last sector of each is its palette, 256x4 at
        (0, 240), the others stream to VRAM (0, 256)."""
        for mode, (stride, first, count) in enumerate(((0x21, 0, 50), (0x51, 0x672, 42), (0x71, 0x13BC, 13))):
            for index in range(count):
                base = (0x21D5 + first + index * stride) * SECTOR
                palette = base + (stride - 1) * SECTOR
                self.sheets.append(Sheet(WA, base, stride - 1, 0, 0x100, f"scenes/m{mode}-{index:02d}",
                                         f"story picture {mode}-{index} (image id 0x{mode:x}{index // 10:x}{index % 10:x})",
                                         [(8, palette)], f"scenes-m{mode}"))

    def emit_sheets(self) -> None:
        for sheet in self.sheets:
            columns = (sheet.sectors + 15) // 16
            for column in range(columns):
                rows = min(16, sheet.sectors - column * 16) * 16
                offset = sheet.base + column * COLUMN
                x = sheet.x + column * 64
                for bpp, clut in sheet.column_variants(column):
                    entries = {4: 16, 8: 256, 16: 0}[bpp]
                    name = f"{sheet.stem}-c{column}-{bpp}" + (f"-p{clut:x}" if entries else "")
                    self.image(sheet.archive, offset, 64, rows, bpp, clut if entries else None, f"{name}.png",
                               f"{sheet.alias}, column {column} at VRAM ({x}, {sheet.y}), {bpp} bpp"
                               + (f", palette at 0x{clut:x}" if entries else ""),
                               clut_entries=entries, stride=64)

    def save_manifest(self) -> None:
        with open(os.path.join(self.out, "manifest.json"), "w", encoding="utf-8") as handle:
            json.dump(self.manifest, handle, indent=1)


FAMILIES = {"cards": Extractor.cards, "portraits": Extractor.portraits, "sheets": Extractor.sheets_family,
            "scenes": Extractor.scenes_family}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", default="game/DATA")
    parser.add_argument("--out", default="tmp/pc/images")
    parser.add_argument("--names", help="cards.tsv with card_number and name columns")
    parser.add_argument("--variants", action="append", default=[],
                        help="assets.txt of a MEMORIES_DUMP_TEXTURES run: the readings it made of the sheets")
    parser.add_argument("--assets", help="assets.txt of a MEMORIES_DUMP_TEXTURES run: extract what it drew")
    parser.add_argument("families", nargs="*", default=list(FAMILIES))
    args = parser.parse_args()
    names: dict[int, str] = {}
    if args.names:
        with open(args.names, encoding="utf-8") as handle:
            names = {int(row["card_number"]): row["name"] for row in csv.DictReader(handle, delimiter="\t")}
    extractor = Extractor(args.data, args.out)
    if args.assets and args.families == list(FAMILIES):
        args.families = []
    for family in args.families:
        if family not in FAMILIES:
            print(f"unknown family {family}; known: {', '.join(FAMILIES)}", file=sys.stderr)
            return 2
        FAMILIES[family](extractor, names) if family == "cards" else FAMILIES[family](extractor)
        if family in ("cards", "portraits"):
            print(f"{family}: {len(extractor.manifest)} images so far")
    if extractor.sheets:
        for listing in args.variants:
            print(f"{listing}: {extractor.variants(listing)} readings added")
        print(f"shared among records of a kind: {extractor.share_variants()} readings")
        extractor.emit_sheets()
        print(f"sheets: {len(extractor.manifest)} images so far ({len(extractor.identical)} files)")
    if args.assets:
        before = len(extractor.manifest)
        extractor.assets(args.assets)
        print(f"assets: {len(extractor.manifest) - before} images")
    extractor.save_manifest()
    print(f"manifest: {os.path.join(args.out, 'manifest.json')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
