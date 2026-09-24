#!/usr/bin/env python3
"""The game's text as an editable listing, and back (notes/translation.md).

    python3 tools/pc/text_listing.py extract [--exe game/rpg-yfm.bin] [-o text.txt]
    python3 tools/pc/text_listing.py check   [--exe game/rpg-yfm.bin]

`extract` writes every string of the three text banks the executable carries
(dialogue and menus, card descriptions, and card, type and duelist names) as
UTF-8 text with the control codes spelled out, which is the file a
translation mod edits and ships (`"text"` in mod.json). `check` extracts,
assembles the listing again at the retail addresses and compares the bytes
with the executable's, which is what shows the listing loses nothing.

The grammar is the one src/pc/text/listing.c compiles; notes/translation.md
describes it for translators.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE_BASE = 0x80010000
EXE_HEADER = 0x800

GLYPH_TABLE = 0x801D9000
GLYPH_COUNT = 0x5C
STRING_TABLE = 0x801C0000   # u16 offsets: menus 0x000-0x0FF, descriptions 0x100+, dialogue 0x400+
NAME_TABLE = 0x801D5800     # u16 offsets from 0x801D0000, ids 0x8000+

# Strings the game writes while it runs (the player's name and the two
# names a two-player screen loads): their bytes are a placeholder, and a
# reference to one must reach the game's own buffer.
BUFFERS = {0x801B125A, 0x801B122B, 0x801B1238}

# Characters the retail glyphs are written as, where the glyph table's
# Shift-JIS is not the character a translator would type.
SPELLING = {0x1B: "'", 0x40: '"', 0x27: "«", 0x28: "»", 0x30: "-", 0x47: "·"}
# More ways to type a retail glyph.
ALIASES = {"’": 0x1B, "‘": 0x1B, "”": 0x40, "“": 0x40, "−": 0x30, "–": 0x30, "《": 0x27, "》": 0x28, "・": 0x47}


def executable_from_disc(path: Path) -> bytes:
    """SLUS_014.11 out of a disc image: raw 2352-byte sectors (a .bin) or an
    ISO of 2048-byte ones, through the ISO 9660 root directory."""
    data = path.read_bytes()
    for size, skip in ((2352, 24), (2352, 16), (2048, 0)):
        if len(data) % size == 0 and data[16 * size + skip + 1:16 * size + skip + 6] == b"CD001":
            break
    else:
        raise SystemExit(f"{path}: not a disc image this tool can read")

    def sector(lba: int) -> bytes:
        at = lba * size + skip
        return data[at:at + 2048]

    root = sector(16)[156:156 + 34]
    lba, length = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    directory = b"".join(sector(lba + i) for i in range((length + 2047) // 2048))
    at = 0
    while at < len(directory):
        record = directory[at]
        if not record:
            at = (at // 2048 + 1) * 2048
            continue
        name = directory[at + 33:at + 33 + directory[at + 32]].decode("ascii", "replace")
        if name.split(";")[0].upper() == "SLUS_014.11":
            start, size_bytes = struct.unpack_from("<I", directory, at + 2)[0], struct.unpack_from("<I", directory, at + 10)[0]
            return b"".join(sector(start + i) for i in range((size_bytes + 2047) // 2048))[:size_bytes]
        at += record
    raise SystemExit(f"{path}: no SLUS_014.11 on the disc")


class Image:
    def __init__(self, path: Path):
        if path.suffix.lower() in (".bin", ".iso", ".img"):
            self.data = executable_from_disc(path)
        else:
            self.data = path.read_bytes()

    def bytes(self, address: int, count: int) -> bytes:
        at = address - EXE_BASE + EXE_HEADER
        return self.data[at:at + count]

    def u16(self, address: int) -> int:
        return struct.unpack("<H", self.bytes(address, 2))[0]

    def u32(self, address: int) -> int:
        return struct.unpack("<I", self.bytes(address, 4))[0]


def glyph_characters(image: Image) -> dict[int, str]:
    table = {0: " "}
    for code in range(1, GLYPH_COUNT):
        sjis = image.u32(GLYPH_TABLE + code * 4) & 0xFFFF
        if not sjis:
            continue
        if code in SPELLING:
            table[code] = SPELLING[code]
            continue
        table[code] = unicodedata.normalize("NFKC", bytes([sjis >> 8, sjis & 0xFF]).decode("shift_jis"))
    return table


# --- banks -------------------------------------------------------------

class Bank:
    """One 64 KB text bank: its base, and the string ids that start in it."""

    def __init__(self, name: str, base: int):
        self.name = name
        self.base = base
        self.ids: dict[int, int] = {}   # id -> bank offset


def banks(image: Image) -> list[Bank]:
    dialog = Bank("dialog", 0x801B0000)
    descriptions = Bank("descriptions", 0x801C0000)
    names = Bank("names", 0x801D0000)
    for index in range(0x100):
        dialog.ids[index] = image.u16(STRING_TABLE + index * 2)
    for index in range(0x400, 0x4FA):   # story strings 0x500-0x5F9; the rest of the table is not
        dialog.ids[index + 0x100] = image.u16(STRING_TABLE + index * 2)
    for index in range(0x100, 0x400):
        descriptions.ids[0xD000 + index] = image.u16(STRING_TABLE + index * 2)
    for index in range(0x360):
        names.ids[0x8000 + index] = image.u16(NAME_TABLE + index * 2)
    return [dialog, descriptions, names]


# --- decoding ----------------------------------------------------------

class DecodeError(Exception):
    pass


class Op:
    """One decoded item: a glyph or a control, its bank offset and length,
    and the listing text it is written as (targets filled in later)."""

    def __init__(self, offset: int, length: int, kind: str, value=None, targets=()):
        self.offset = offset
        self.length = length
        self.kind = kind          # "glyph", "nl", "end", "jump", "code"
        self.value = value
        self.targets = list(targets)


def decode_bank(image: Image, bank: Bank, glyphs: dict[int, str]) -> tuple[dict[int, Op], set[int]]:
    """Every op reachable from the bank's strings, by offset, and the offsets
    something jumps to."""
    memory = image.bytes(bank.base, 0x10000)
    ops: dict[int, Op] = {}
    targets: set[int] = set()
    work = sorted(set(offset for offset in bank.ids.values() if offset))
    starts = set(work)
    setups: dict[int, int] = {}   # choice setups: offset -> number of choices
    deferred: list[int] = []

    def byte(at: int) -> int:
        if at >= len(memory):
            raise DecodeError(f"{bank.name}: past the bank at {at:#x}")
        return memory[at]

    def word(at: int) -> int:
        return byte(at) | byte(at + 1) << 8

    while work or deferred:
        if not work:
            # Paths that met a choice jump before any setup: its setup is
            # the nearest one before it, now that everything else is read.
            waiting, deferred = deferred, []
            progress = False
            for entry in waiting:
                if any(offset < entry for offset in setups):
                    work.append(entry)
                    progress = True
                else:
                    deferred.append(entry)
            if not progress:
                raise DecodeError(f"{bank.name}: choice jumps with no choice before them: "
                                  + ", ".join(f"{e:#x}" for e in deferred))
            continue
        at = work.pop()
        entry = at
        choices = None
        while at not in ops:
            if bank.base + at in BUFFERS:
                break
            op = byte(at)
            here = at
            follow = True
            if op < 0xF0:
                item = Op(here, 1, "glyph", op)
            elif op <= 0xF5:
                item = Op(here, 2, "glyph", ((op - 0xF0) << 8) | byte(at + 1))
            elif op == 0xF6:
                item = Op(here, 3, "code", f"fx {byte(at + 1):02X} {byte(at + 2):02X}")
            elif op == 0xF7:
                # Some states read operands of their own once they start
                # (duel_effect_state_callbacks.c): an image or camera move
                # (5, 0x0B), a pause and a shake (0x0F, 0x10).
                state = byte(at + 1)
                count = {0x05: 2, 0x0B: 6, 0x0F: 2, 0x10: 2}.get(state & 0x1F, 0)
                if state & 0x1F == 0x05 and word(at + 2) & 0x8000:
                    count = 5
                data = "".join(f" {byte(at + 2 + i):02X}" for i in range(count))
                item = Op(here, 2 + count, "code", f"state {state:02X}{data}")
            elif op == 0xF8:
                item, follow, choices = decode_secondary(memory, at, choices, starts)
            elif op == 0xF9:
                command = word(at + 1)
                if command & 0x4000:
                    item = Op(here, 3, "code", f"set {command:04X}")
                else:
                    item = Op(here, 5, "code", f"if {command:04X}", [word(at + 3)])
            elif op == 0xFA:
                item = Op(here, 1, "code", "page")
            elif op == 0xFB:
                control = byte(at + 1)
                length = 2
                text = f"choice {control:02X}"
                if control & 0x08:
                    text += f" {byte(at + 2):02X}"
                    length = 3
                if control & 0x80:
                    if choices is None:
                        before = [offset for offset in setups if offset < at]
                        if not before:
                            deferred.append(entry)
                            break
                        choices = setups[max(before)]
                    table = jump_table(memory, at + length, choices, starts)
                    count = len(table)
                    item = Op(here, length + 2 * count, "code", text.replace("choice", "choose", 1), table)
                    follow = False
                else:
                    choices = control & 7
                    setups[here] = choices
                    item = Op(here, length, "code", text)
            elif op == 0xFC:
                item = Op(here, 3, "code", "call", [word(at + 1)])
            elif op == 0xFD:
                item = Op(here, 3, "code", "jump", [word(at + 1)])
                follow = False
            elif op == 0xFE:
                item = Op(here, 1, "nl")
            else:
                item = Op(here, 1, "end")
                follow = False
            for other in range(here + 1, here + item.length):
                if other in ops or other in starts:
                    raise DecodeError(f"{bank.name}: {here:#x} overlaps {other:#x}")
            ops[here] = item
            for target in item.targets:
                if not target:
                    continue   # a null entry, never taken
                targets.add(target)
                if target not in ops:
                    work.append(target)
            if not follow:
                break
            at = here + item.length
    for target in targets:
        if bank.base + target in BUFFERS:
            continue
        if target not in ops:
            raise DecodeError(f"{bank.name}: jump into the middle of an op at {target:#x}")
    return ops, targets


def jump_table(memory: bytes, at: int, most: int, starts: set[int]) -> list[int]:
    """Up to `most` u16 targets at `at`. A table is often shorter than the
    command allows: it ends where its first target, or another string,
    begins (the text a choice jumps to usually follows its table)."""
    table: list[int] = []
    end = at + 2 * most
    for i in range(most):
        here = at + 2 * i
        if here >= end or here in starts or here + 1 in starts:
            break
        target = memory[here] | memory[here + 1] << 8
        table.append(target)
        if target and at < target < end:
            end = target
    return table


def decode_secondary(memory: bytes, at: int, choices, starts):
    """F8 and its operands: (op, whether the stream goes on, choice count)."""
    index = memory[at + 1]
    operands = {0x00: 1, 0x01: 1, 0x02: 1, 0x03: 5, 0x04: 1, 0x05: 2, 0x06: 2, 0x07: 2, 0x08: 0, 0x09: 0,
                0x0A: 1, 0x0B: 1, 0x0C: 1, 0x0D: 6, 0x0E: 2, 0x11: 1, 0x12: 0, 0x13: 0, 0x14: 1, 0x15: 1,
                0x16: 0, 0x19: 1, 0x1A: 0, 0x1B: 1, 0x1C: 1, 0x1D: 1, 0x1E: 1, 0x1F: 1, 0x20: 1,
                0x21: 2, 0x22: 1, 0x25: 0, 0x27: 2, 0x28: 2, 0x29: 0, 0x2A: 0}
    if index == 0x0F:
        count = 1 + (2 if memory[at + 2] & 0x3F else 0)
    elif index == 0x10:
        count = 2 + (2 if memory[at + 3] & 0x80 else 0)
    elif index in (0x17, 0x18):
        table = jump_table(memory, at + 2, 4, starts)
        return Op(at, 2 + 2 * len(table), "code", f"f8 {index:02X}", table), False, choices
    elif index in operands:
        count = operands[index]
    else:
        raise DecodeError(f"F8 {index:02X} at {at:#x} is not a text command")
    if index in (0x27, 0x28):
        target = memory[at + 2] | memory[at + 3] << 8
        return Op(at, 4, "code", f"f8 {index:02X}", [target]), index != 0x28, choices
    data = " ".join(f"{b:02X}" for b in memory[at + 2:at + 2 + count])
    return Op(at, 2 + count, "code", f"f8 {index:02X}" + (" " + data if data else "")), index != 0x2A, choices


# --- writing the listing -------------------------------------------------

def label(bank: Bank, target: int, starts: dict[int, list[int]]) -> str:
    # A buffer's label is never defined in the listing: the compiler takes
    # an undefined label for the retail address, which is the game's buffer.
    return f"L{target:04X}" if target else "0"


def card_names(image: Image, glyphs: dict[int, str]) -> dict[int, str]:
    """The retail card names as plain text, for the descriptions' comments."""
    names = {}
    for card in range(1, 723):
        at = 0x801D0000 + image.u16(NAME_TABLE + card * 2)
        text = ""
        while len(text) < 64:
            code = image.bytes(at, 1)[0]
            if code >= 0xF0:
                break
            text += glyphs.get(code, "?")
            at += 1
        names[card] = text
    return names


def write_listing(image: Image) -> str:
    glyphs = glyph_characters(image)
    names = card_names(image, glyphs)
    out = [
        "# Yu-Gi-Oh! Forbidden Memories text listing (notes/translation.md).",
        "# [ID] starts a string; its text runs to {end}. A line break in the text",
        "# is a line break in the game's text box. {...} are the game's control",
        "# codes: keep them, move them with the words they belong to.",
        "",
    ]
    for bank in banks(image):
        ops, targets = decode_bank(image, bank, glyphs)
        starts: dict[int, list[int]] = {}
        for string_id, offset in sorted(bank.ids.items()):
            if offset:
                starts.setdefault(offset, []).append(string_id)
        out.append(f"@bank {bank.name}")
        out.append("")
        inside = False
        previous_end = None
        for offset in sorted(set(ops) | set(o for o in starts if bank.base + o in BUFFERS)):
            op = ops.get(offset)
            header = offset in starts
            marker = offset in targets and not header
            if header or marker or (previous_end is not None and offset != previous_end):
                if inside:
                    out[-1] += "{cont}"   # the text runs on into the next item
                ids = " ".join(f"{i:04X}" for i in starts.get(offset, []))
                if header:
                    comment = ""
                    if bank.name == "descriptions":
                        card = starts[offset][0] - 0xD100
                        if card in names:
                            comment = "  # " + names[card]
                    out.append(f"[{ids}]{comment}")
                    if bank.base + offset in BUFFERS:
                        out.append("{buffer}")
                        out.append("")
                        inside = False
                        previous_end = None
                        continue
                    if offset in targets:
                        out[-1] += f"  {{:L{offset:04X}}}"
                else:
                    out.append(f"{{:L{offset:04X}}}")
                out.append("")
                inside = True
            text = out[-1]
            if op.kind == "glyph":
                character = glyphs.get(op.value)
                if character is None or op.value >= 0x100:
                    text += f"{{g {op.value:X}}}"
                elif character == " " and (offset + 1 not in ops or ops[offset + 1].kind in ("nl", "end")):
                    text += "{sp}"
                else:
                    text += character
                out[-1] = text
            elif op.kind == "nl":
                nxt = offset + 1
                if nxt in starts or nxt in targets or nxt not in ops:
                    out[-1] = text + "{nl}"
                else:
                    out.append("")
            elif op.kind == "end":
                out[-1] = text + "{end}"
                out.append("")
                inside = False
            else:
                words = op.value
                if op.targets:
                    words += " " + " ".join(label(bank, t, starts) for t in op.targets)
                out[-1] = text + "{" + words + "}"
                if op.kind == "code" and op.value == "jump" or op.value.startswith("choose") or \
                        op.value.startswith("f8 17") or op.value.startswith("f8 18") or op.value == "f8 28":
                    out.append("")
                    inside = False
            previous_end = offset + op.length
            if bank.name == "names" and op.kind == "end":
                # The card names, for the descriptions' comments.
                pass
        if bank.name == "names":
            pass
        out.append("")
        if bank.name == "dialog":
            pass
    return "\n".join(out) + "\n"


# --- reading it back ------------------------------------------------------
#
# The grammar, as listing.c reads it too:
#   @bank NAME            the bank the following items belong to
#   [ID ID...]            a string (several ids may share it); its text starts
#                         on the next line and runs to {end}, {jump}, {choose},
#                         {f8 17}, {f8 18} or {f8 28}, after which lines are
#                         ignored up to the next item
#   {:LXXXX}              a place something jumps to (XXXX its retail offset);
#                         on a line of its own it starts an item, like [ID]
#   text                  glyphs; a line break is a line break in the game,
#                         except the one before a line that starts an item
#   {code ...}            a control code; {nl} a line break, {sp} a space,
#                         {cont} nothing (the text runs on into the next item)
#   # ...                 a comment, between items only

TOKEN = re.compile(r"\{([^{}]*)\}|.", re.S)
TERMINATORS = ("buffer", "end", "jump", "choose", "f8 17", "f8 18", "f8 28")


def code_bytes(words: list[str], labels) -> bytes:
    """A control code's bytes; `labels` turns a target into its u16."""
    name = words[0]

    def hexes(items):
        return bytes(int(w, 16) for w in items)

    def target(word):
        value = labels(word)
        return bytes([value & 0xFF, value >> 8])

    if name == "nl":
        return b"\xFE"
    if name == "end":
        return b"\xFF"
    if name == "page":
        return b"\xFA"
    if name == "sp":
        return b"\x00"
    if name in ("cont", "buffer"):
        return b""
    if name == "g":
        code = int(words[1], 16)
        return bytes([code]) if code < 0xF0 else bytes([0xF0 + (code >> 8), code & 0xFF])
    if name == "fx":
        return b"\xF6" + hexes(words[1:3])
    if name == "state":
        return b"\xF7" + hexes(words[1:])
    if name == "set":
        value = int(words[1], 16)
        return bytes([0xF9, value & 0xFF, value >> 8])
    if name == "if":
        value = int(words[1], 16)
        return bytes([0xF9, value & 0xFF, value >> 8]) + target(words[2])
    if name in ("choice", "choose"):
        control = int(words[1], 16)
        out = bytes([0xFB, control])
        rest = words[2:]
        if control & 0x08:
            out += hexes(rest[:1])
            rest = rest[1:]
        for word in rest:
            out += target(word)
        return out
    if name == "call":
        return b"\xFC" + target(words[1])
    if name == "jump":
        return b"\xFD" + target(words[1])
    if name == "f8":
        index = int(words[1], 16)
        out = bytes([0xF8, index])
        if index in (0x17, 0x18, 0x27, 0x28):
            for word in words[2:]:
                out += target(word)
            return out
        return out + hexes(words[2:])
    raise ValueError(f"unknown code {{{' '.join(words)}}}")


def parse(listing: str, encode_glyph):
    """The listing's items: (bank, ids, labels at its start, bytes-with-
    fixups). A fixup is (position, target word)."""
    items = []
    bank = None
    current = None
    lines = listing.split("\n")
    for number, raw in enumerate(lines):
        line = raw.rstrip("\r")
        starts_item = line.startswith("[") or line.startswith("{:")
        if current is not None and starts_item:
            # The line break before an item is the listing's, not the game's:
            # the pending one is simply not written.
            current = None
        if current is None:
            if line.startswith("@bank "):
                bank = line.split()[1]
            elif starts_item:
                head = line
                ids = []
                if head.startswith("["):
                    close = head.index("]")
                    ids = [int(w, 16) for w in head[1:close].split()]
                    head = head[close + 1:]
                labels = re.findall(r"\{:(L[0-9A-Fa-f]{4})\}", head.split("#")[0])
                current = {"bank": bank, "ids": ids, "labels": labels, "data": bytearray(), "fixups": [],
                           "pending_newline": False, "line": number + 1}
                items.append(current)
                if not line.startswith("[") :
                    # {:L} on its own line: text may follow on the same line.
                    rest = re.sub(r"^(\{:L[0-9A-Fa-f]{4}\} *)+", "", line)
                    if rest:
                        consume(current, rest, encode_glyph)
                        if current.get("done"):
                            current = None
                        else:
                            current["pending_newline"] = True
                continue
            continue
        if current["pending_newline"]:
            current["data"].append(0xFE)
        current["pending_newline"] = False
        consume(current, line, encode_glyph)
        if current.get("done"):
            current = None
            continue
        current["pending_newline"] = True
    return items


def consume(item, text: str, encode_glyph):
    for match in TOKEN.finditer(text):
        if item.get("done"):
            if match.group(0).strip():
                raise ValueError(f"line {item['line']}: text after the end of a string: {text!r}")
            continue
        if match.group(1) is not None:
            words = match.group(1).split()
            if not words:
                raise ValueError("empty {}")
            if words[0].startswith(":"):
                raise ValueError("a label inside a line")
            data = code_bytes(words, lambda word: 0)
            for position, word in target_positions(words, len(item["data"])):
                item["fixups"].append((position, word))
            item["data"] += data
            if " ".join(words[:2]) in TERMINATORS or words[0] in TERMINATORS:
                item["done"] = True
        elif match.group(0) == "\n":
            item["data"].append(0xFE)
        else:
            item["data"] += bytes(encode_glyph(match.group(0)))


def target_positions(words: list[str], at: int):
    """Where in a code's bytes its targets go, and their words."""
    name = words[0]
    if name in ("call", "jump"):
        return [(at + 1, words[1])]
    if name == "if":
        return [(at + 3, words[2])]
    if name in ("choice", "choose"):
        control = int(words[1], 16)
        first = 3 if control & 0x08 else 2
        rest = words[3:] if control & 0x08 else words[2:]
        return [(at + first + 2 * i, word) for i, word in enumerate(rest)]
    if name == "f8" and int(words[1], 16) in (0x17, 0x18, 0x27, 0x28):
        return [(at + 2 + 2 * i, word) for i, word in enumerate(words[2:])]
    return []


def glyph_encoder(image: Image):
    glyphs = glyph_characters(image)
    reverse = {character: code for code, character in sorted(glyphs.items(), reverse=True)}
    reverse.update(ALIASES)

    def encode(character: str) -> list[int]:
        if character not in reverse:
            raise ValueError(f"no glyph for {character!r}")
        code = reverse[character]
        return [code] if code < 0xF0 else [0xF0 + (code >> 8), code & 0xFF]
    return encode


def check(image: Image) -> int:
    listing = write_listing(image)
    items = parse(listing, glyph_encoder(image))
    memory = {bank.name: (bank, image.bytes(bank.base, 0x10000)) for bank in banks(image)}
    problems = 0
    compared = 0
    for item in items:
        bank, retail = memory[item["bank"]]
        if item["ids"]:
            start = bank.ids[item["ids"][0]]
        else:
            start = int(item["labels"][0][1:], 16)
        data = bytearray(item["data"])
        for position, word in item["fixups"]:
            value = 0 if word == "0" else int(word[1:], 16)
            data[position:position + 2] = bytes([value & 0xFF, value >> 8])
        if bank.base + start in BUFFERS:
            continue
        if bytes(data) != retail[start:start + len(data)]:
            problems += 1
            if problems <= 10:
                print(f"{item['bank']} {start:#06x} (line {item['line']}): differs\n"
                      f"  listing {bytes(data).hex(' ')}\n  retail  {retail[start:start + len(data)].hex(' ')}")
        compared += len(data)
    print(f"check: {len(items)} items, {compared} bytes compared, {problems} differ")
    return 1 if problems else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["extract", "check"])
    parser.add_argument("--exe", type=Path, help="SLUS_014.11, or the disc image it is on (default: the one in game/)")
    parser.add_argument("-o", "--output", type=Path)
    arguments = parser.parse_args()
    if not arguments.exe:
        found = sorted((ROOT / "game").glob("SLUS_014.11")) or sorted((ROOT / "game").glob("*.bin"))
        if not found:
            raise SystemExit("no game/SLUS_014.11 or disc image in game/; name one with --exe")
        arguments.exe = found[0]
    image = Image(arguments.exe)
    if arguments.command == "extract":
        text = write_listing(image)
        if arguments.output:
            arguments.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        return 0
    return check(image)


if __name__ == "__main__":
    sys.exit(main())
