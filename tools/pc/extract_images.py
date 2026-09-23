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

Every PNG is listed in <out>/manifest.json with its provenance: archive,
byte offset, size in VRAM words and rows, depth, and the palette's offset,
which is the identity a texture pack goes by. Names are aliases on top.

Usage: extract_images.py [--data game/DATA] [--out tmp/pc/images]
                         [--names cards.tsv] [family ...]
cards.tsv (optional, columns card_number and name) adds the card's name to
its file name.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import struct
import sys
import zlib

SECTOR = 2048


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
    r = ((colour & 0x1F) * 255 + 15) // 31  # as texture_dump.c expands a word
    g = (((colour >> 5) & 0x1F) * 255 + 15) // 31
    b = (((colour >> 10) & 0x1F) * 255 + 15) // 31
    return bytes((r, g, b, 255))


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


class Extractor:
    def __init__(self, data_dir: str, out: str) -> None:
        self.data_dir = data_dir
        self.out = out
        self.manifest: list[dict] = []
        self.archives: dict[str, bytes] = {}

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
        palette = read_palette(data, clut_offset, entries) if entries else None
        if palette is not None and len(palette) < 256:
            palette = palette + [0] * (256 - len(palette))  # indices past a short palette
        width, height, rgba = decode(data, offset, words, rows, bpp, palette, stride, row_offsets)
        if crop and (crop[0] or crop[1] != width):
            left, span = crop
            rgba = b"".join(rgba[(y * width + left) * 4:(y * width + left + span) * 4] for y in range(height))
            width = span
        full = os.path.join(self.out, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        write_png(full, width, height, rgba)
        self.manifest.append({
            "file": path.replace(os.sep, "/"), "alias": alias, "archive": archive, "offset": offset,
            "words": words, "rows": rows, "bpp": bpp, "width": width, "height": height,
            "clut_offset": clut_offset, "clut_entries": entries, "stride": words if stride is None else stride,
            "row_offsets": row_offsets,
        })

    def assets(self, listing: str) -> None:
        """assets.txt from a MEMORIES_DUMP_TEXTURES run: every texture the game
        drew whose words and palette were traced to the disc."""
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

    def cards(self, names: dict[int, str]) -> None:
        """func_800289BC: seven sectors per card from WA sector 722."""
        for card in range(1, 723):
            base = ((card - 1) * 7 + 722) * SECTOR
            stem = f"{card:04d}"
            if card in names:
                stem += "-" + re.sub(r"[^a-z0-9]+", "-", names[card].lower()).strip("-")
            clut = base + 0x2640
            self.image("WA_MRG.MRG", base, 0x33, 0x60, 8, clut, f"cards/{stem}.png", f"card {card} art")
            self.image("WA_MRG.MRG", base + 0x2840, 0x18, 0x0E, 8, clut, f"cards/{stem}.strip.png",
                       f"card {card} strip below")
            self.image("WA_MRG.MRG", base + 0x2AE0, 0x08, 0x58, 8, clut, f"cards/{stem}.side.png",
                       f"card {card} strip beside")

    def portraits(self) -> None:
        """Campaign_LoadScenePackage and FreeDuel_Init: 0x980-byte records."""
        for bank, base, count in (("campaign", 0xF35000, 25), ("freeduel", 0xF55000, 40)):
            for index in range(count):
                record = base + index * 0x980
                self.image("WA_MRG.MRG", record, 24, 48, 8, record + 0x900, f"portraits/{bank}-{index:02d}.png",
                           f"{bank} portrait {index} (F6 id 0x{0x41 + index:02x})" if bank == "campaign"
                           else f"free duel portrait {index}", clut_entries=64)

    def save_manifest(self) -> None:
        with open(os.path.join(self.out, "manifest.json"), "w", encoding="utf-8") as handle:
            json.dump(self.manifest, handle, indent=1)


FAMILIES = {"cards": Extractor.cards, "portraits": Extractor.portraits}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", default="game/DATA")
    parser.add_argument("--out", default="tmp/pc/images")
    parser.add_argument("--names", help="cards.tsv with card_number and name columns")
    parser.add_argument("--assets", help="assets.txt of a MEMORIES_DUMP_TEXTURES run: extract what it drew")
    parser.add_argument("families", nargs="*", default=list(FAMILIES))
    args = parser.parse_args()
    names: dict[int, str] = {}
    if args.names:
        with open(args.names, encoding="utf-8") as handle:
            names = {int(row["card_number"]): row["name"] for row in csv.DictReader(handle, delimiter="\t")}
    extractor = Extractor(args.data, args.out)
    if args.assets:
        extractor.assets(args.assets)
        print(f"assets: {len(extractor.manifest)} images")
        args.families = [] if args.families == list(FAMILIES) else args.families
    for family in args.families:
        if family not in FAMILIES:
            print(f"unknown family {family}; known: {', '.join(FAMILIES)}", file=sys.stderr)
            return 2
        FAMILIES[family](extractor, names) if family == "cards" else FAMILIES[family](extractor)
        print(f"{family}: {len(extractor.manifest)} images so far")
    extractor.save_manifest()
    print(f"manifest: {os.path.join(args.out, 'manifest.json')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
