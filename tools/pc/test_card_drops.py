#!/usr/bin/env python3
"""Live Game > Card drops test (built game and user-supplied disc).

Wins a real duel (the smoke test's opening, then the opponent's life points
set to zero in a saved state), and on RESULTS OF DUEL checks the page order,
the cards awarded on leaving, and that a state saved on the screen keeps the
added pages. Artifacts, settings, states and screenshots stay in
tmp/pc/card-drops-smoke; no player saves are touched.

    python3 tools/pc/test_card_drops.py [--windows]
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tmp/pc/card-drops-smoke"
EXECUTABLE = ROOT / "tmp/pc/game32/memories-pc"
WINDOWS_EXECUTABLE = ROOT / "tmp/pc/win32/memories-pc.exe"
WINE_PREFIX = ROOT / "tmp/pc/wine-prefix"

HAND = 6560      # the duel's first hand (tests/pc/smoke/duel-hand-camera.json)
RESULTS = 8400   # RESULTS OF DUEL is up and the drops are dealt
# Fuse two hand cards, set the result, pick its guardian star and end the
# turn: the zeroed life points end the duel.
WIN = ("6570:0010,6576:0000,6590:0020,6596:0000,6610:0010,6616:0000,6640:8000,6646:0000,"
       "6700:4000,6706:0000,7600:4000,7606:0000,7800:0008,7806:0000")
RIGHT, LEFT, CROSS = "0020", "0080", "4000"   # the script's pad bits

OPPONENT_LP = 0x800EA024        # D_800E9FF0[1].life_points
OPPONENT_SHOWN_LP = 0x800EA022  # and the value drawn
RESULT_RECORD = 0x8009B1E8      # D_8009B1E8, DuelResultDisplayState *
PAGE_INDEX, DROPPED_CARD = 0x37, 0x3C
CHEST = 0x801D0250 - 1          # gLibrary_abCardChest, by card id
RECENT = 0x801D07BC             # the 16 cards last awarded, newest first


def chunks(path):
    data = bytearray(path.read_bytes())
    at, found = 16, {}
    while at + 20 <= len(data):
        tag = data[at:at + 16].split(b"\0")[0].decode()
        size, = struct.unpack_from("<I", data, at + 16)
        found[tag] = at + 20
        at += 20 + size
    return data, found["memory"]


def peek(path, address, form):
    data, memory = chunks(path)
    return struct.unpack_from("<" + form, data, memory + address - 0x80000000)[0]


def chest(path):
    data, memory = chunks(path)
    return data[memory + CHEST + 1 - 0x80000000:memory + CHEST + 723 - 0x80000000]


def presses(frame, keys):
    return ",".join(f"{frame + 40 * i}:{key},{frame + 40 * i + 6}:0000" for i, key in enumerate(keys))


def run(executable, label, frame, sequence="", state=None, drops=1):
    folder = OUT / label
    folder.mkdir(parents=True, exist_ok=True)
    for old in folder.glob("*.bmp"):
        old.unlink()
    settings = folder / "settings.txt"
    settings.write_text(f"card_drops={drops}\nscale=2\nmod.3d-monsters=0\nmod.hand-camera=0\nmod.ai-hard-mode=0\n")
    env = {key: value for key, value in os.environ.items() if not key.startswith("MEMORIES_")}
    env.update(SDL_VIDEODRIVER=os.environ.get("SDL_VIDEODRIVER", "offscreen"),
               MEMORIES_DETERMINISTIC="1", MEMORIES_NO_AUDIO="1", MEMORIES_NO_GAMEPAD="1",
               MEMORIES_SPEED="-1", MEMORIES_SETTINGS=str(settings), MEMORIES_USER_DIR=str(folder / "user"),
               MEMORIES_INPUT=sequence, MEMORIES_WINDOW_SHOT=str(frame), MEMORIES_DUMP_FRAME=str(frame + 1),
               MEMORIES_SCREENSHOT_DIR=str(folder), MEMORIES_DUMP_PATH=str(folder / "game.ppm"),
               MEMORIES_SAVE_STATE=f"{frame}:{folder / 'end.state'}")
    if "MEMORIES_DISC" in os.environ:
        env["MEMORIES_DISC"] = os.environ["MEMORIES_DISC"]
    if state:
        env["MEMORIES_LOAD_STATE"] = str(state)
    command = [str(executable)]
    if executable.suffix == ".exe":
        command = ["wine", str(executable)]
        env.update(WINEPREFIX=str(WINE_PREFIX), WINEDLLOVERRIDES="mscoree,mshtml=", WINEDEBUG="-all")
    with (folder / "run.log").open("w") as log:
        subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    return folder / "end.state"


def record(path):
    return peek(path, RESULT_RECORD, "I")


def page(path):
    return peek(path, record(path) + PAGE_INDEX, "B")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--windows", action="store_true", help="test the Windows build under Wine")
    executable = WINDOWS_EXECUTABLE if parser.parse_args().windows else EXECUTABLE
    opening = json.loads((ROOT / "tests/pc/smoke/duel-hand-camera.json").read_text())["input"]
    hand = run(executable, "hand", HAND, opening)
    data, memory = chunks(hand)
    for address in (OPPONENT_LP, OPPONENT_SHOWN_LP):
        struct.pack_into("<h", data, memory + address - 0x80000000, 0)
    ending = OUT / "ending.state"
    ending.write_bytes(data)
    before = chest(ending)

    # One card: the console's three pages and one award.
    one = run(executable, "one-right", RESULTS + 120, WIN + "," + presses(RESULTS + 20, [RIGHT]), ending)
    assert page(one) == 1, "Right from SPOILS should reach the statistics with one card"
    one = run(executable, "one-exit", RESULTS + 900, WIN + "," + presses(RESULTS + 20, [CROSS]), ending)
    first = peek(one, record(one) + DROPPED_CARD, "h")
    gained = [a - b for a, b in zip(chest(one), before)]
    assert sum(gained) == 1 and gained[first - 1] == 1, "one card should award SPOILS' card alone"

    # Twenty: the added pages sit between SPOILS and the statistics.
    many = run(executable, "many", RESULTS, WIN, ending, drops=20)
    right = run(executable, "many-right", RESULTS + 120, presses(RESULTS + 20, [RIGHT]), many, drops=20)
    assert page(right) == 3, "Right from SPOILS should open the first added page"
    left = run(executable, "many-left", RESULTS + 120, presses(RESULTS + 20, [LEFT]), many, drops=20)
    assert page(left) == 2, "Left from SPOILS should still reach SPECIAL ARTS"
    back = run(executable, "many-back", RESULTS + 200, presses(RESULTS + 20, [LEFT, LEFT, LEFT]), many, drops=20)
    assert page(back) >= 3, "Left from the statistics should reach the last added page"
    # Leaving with the setting back at one: the duel's own deal is awarded,
    # from a state saved on the screen, the first card last.
    done = run(executable, "many-exit", RESULTS + 900, presses(RESULTS + 20, [CROSS]), many)
    first = peek(done, record(done) + DROPPED_CARD, "h")
    gained = [a - b for a, b in zip(chest(done), before)]
    assert sum(gained) == 20, f"twenty cards should be awarded, not {sum(gained)}"
    assert peek(done, RECENT, "H") == first, "SPOILS' card should be the last awarded"
    print(f"card drops: page order, awards and saved states passed; {OUT}")


if __name__ == "__main__":
    main()
