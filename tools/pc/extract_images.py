#!/usr/bin/env python3
"""Extract the game's images from the disc archives as PNG, named by origin.

The archives hold no image files: each screen's loader streams raw VRAM
words from a known sector and uploads them as rectangles, with the palette
in another rectangle (notes/mrg-files.md). So an image is defined by its
loader, and this tool replays the loaders it knows, family by family:

  cards      the 722 cards' artwork (func_800289BC: WA_MRG.MRG sector
             (n-1)*7 + 722, seven sectors; the 102x96 8-bit picture, its
             256-entry palette, the strip under it and the strip beside it)

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
    r = (colour & 0x1F) * 255 // 31
    g = ((colour >> 5) & 0x1F) * 255 // 31
    b = ((colour >> 10) & 0x1F) * 255 // 31
    return bytes((r, g, b, 255))


def decode(data: bytes, offset: int, words: int, rows: int, bpp: int, palette: list[int] | None) -> tuple[int, int, bytes]:
    """A rectangle of VRAM words as RGBA pixels; bpp 4/8 index the palette."""
    per_word = {4: 4, 8: 2, 16: 1}[bpp]
    width = words * per_word
    out = bytearray()
    for y in range(rows):
        row = data[offset + y * words * 2:offset + (y + 1) * words * 2]
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
              clut_offset: int | None, path: str, alias: str) -> None:
        data = self.archive(archive)
        entries = {4: 16, 8: 256, 16: 0}[bpp]
        palette = read_palette(data, clut_offset, entries) if entries else None
        width, height, rgba = decode(data, offset, words, rows, bpp, palette)
        full = os.path.join(self.out, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        write_png(full, width, height, rgba)
        self.manifest.append({
            "file": path.replace(os.sep, "/"), "alias": alias, "archive": archive, "offset": offset,
            "words": words, "rows": rows, "bpp": bpp, "width": width, "height": height,
            "clut_offset": clut_offset, "clut_entries": entries,
        })

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

    def save_manifest(self) -> None:
        with open(os.path.join(self.out, "manifest.json"), "w", encoding="utf-8") as handle:
            json.dump(self.manifest, handle, indent=1)


FAMILIES = {"cards": Extractor.cards}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", default="game/DATA")
    parser.add_argument("--out", default="tmp/pc/images")
    parser.add_argument("--names", help="cards.tsv with card_number and name columns")
    parser.add_argument("families", nargs="*", default=list(FAMILIES))
    args = parser.parse_args()
    names: dict[int, str] = {}
    if args.names:
        with open(args.names, encoding="utf-8") as handle:
            names = {int(row["card_number"]): row["name"] for row in csv.DictReader(handle, delimiter="\t")}
    extractor = Extractor(args.data, args.out)
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
