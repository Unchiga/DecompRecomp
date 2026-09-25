#!/usr/bin/env python3
"""Live fusion-helper smoke test (built Linux SDL game and user-supplied disc).

Uses SDL's offscreen GL driver by default. Artifacts, settings, states and
screenshots stay in tmp/pc/fusion-helper-smoke; no player saves are touched.
"""
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tmp/pc/fusion-helper-smoke"
PICKS = "6570:0010,6576:0000,6590:0020,6596:0000,6610:0010,6616:0000"
WRONG = "6570:0010,6576:0000,6590:0020,6596:0000,6602:0020,6608:0000,6620:0010,6626:0000"
SUMMON = PICKS + ",6640:8000,6646:0000,6700:4000,6706:0000"


def memory(path):
    data = path.read_bytes()
    at = 16
    while at + 20 <= len(data):
        tag = data[at:at + 16].split(b"\0")[0]
        size, = struct.unpack_from("<I", data, at + 16)
        if tag == b"memory":
            return data[at + 20:at + 20 + size]
        at += 20 + size
    raise AssertionError(f"No memory chunk in {path}")


TEXT = {b"\xf8\xf6\xf2": "white", b"\xa0\xd6\x8f": "green", b"\x7c\x86\xe8": "red"}


def panel_pixels(path, colour="white"):
    data = path.read_bytes()
    offset, = struct.unpack_from("<I", data, 10)
    width, height = struct.unpack_from("<ii", data, 18)
    bits, = struct.unpack_from("<H", data, 28)
    assert data[:2] == b"BM" and bits in (24, 32)
    stride = ((width * bits + 31) // 32) * 4
    return sum(data[offset + y * stride + x * (bits // 8):
                    offset + y * stride + x * (bits // 8) + 3] == next(k for k, v in TEXT.items() if v == colour)
               for y in range(abs(height)) for x in range(width))


def run(label, frame, sequence="", state=None, mode=1, mods=None, colour="white"):
    folder = OUT / label
    folder.mkdir(parents=True, exist_ok=True)
    settings = folder / "settings.txt"
    settings.write_text(f"fusion_helper={mode}\nscale=4\nmod.3d-monsters=0\n"
                        "mod.hand-camera=0\nmod.ai-hard-mode=0\nmod.free-duel-portraits-hd=0\n")
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
    if mods:
        env["MEMORIES_MODS_DIR"] = str(mods)
    with (folder / "run.log").open("w") as log:
        subprocess.run([str(ROOT / "tmp/pc/game32/memories-pc")], cwd=ROOT, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
    shot = max(folder.glob("*.bmp"), key=lambda p: p.stat().st_mtime_ns)
    return memory(folder / "end.state"), panel_pixels(shot, colour)


def main():
    opening = json.loads((ROOT / "tests/pc/smoke/duel-hand-camera.json").read_text())["input"]
    ram, pixels = run("hand", 6560, opening)
    assert pixels > 100, "Helper did not draw in the human hand phase"
    state = OUT / "hand/end.state"
    on, pixels = run("on", 6600, state=state)
    off, hidden = run("off", 6600, state=state, mode=0)
    assert pixels > 100 and hidden == 0
    assert on[0xFE6F8:0xFE6FC] == off[0xFE6F8:0xFE6FC], "Helper changed game RNG"
    assert on[0x1A7AD8:0x1A7AD8 + 30 * 28] == off[0x1A7AD8:0x1A7AD8 + 30 * 28], "Helper changed cards"
    selected, pixels = run("selected", 6640, PICKS.split(",6590")[0], state)
    assert pixels > 100 and selected[0xEA039] == 1, "Target not white after one right pick"
    _, pixels = run("picked", 6640, PICKS, state, colour="green")
    assert pixels > 100, "Target not green once the picks make it"
    _, pixels = run("wrong", 6660, WRONG, state, colour="red")
    assert pixels > 100, "Target not red once the picks miss it"
    summoned, pixels = run("summoned", 7500, SUMMON, state)
    assert pixels == 0, "Helper remained visible during summoning"
    assert struct.unpack_from("<3h", summoned, 0x1A7AE4) == (531, 2100, 1700)
    _, pixels = run("viewer", 6640, "6570:1000,6576:0000", state)
    assert pixels == 0, "Helper remained visible during card inspection"

    mods = OUT / "mods"
    fixture = mods / "fusion-fixture"
    fixture.mkdir(parents=True, exist_ok=True)
    (fixture / "mod.json").write_text(json.dumps({
        "id": "fusion-fixture", "name": "Fusion helper smoke fixture", "version": "1", "enabled": True,
        "cards": [{"id": "dragon", "copy": 1, "name": "Étoile Dragon", "attack": 4200, "defense": 3100}],
        "fusions": [{"with": [395, 591], "result": "fusion-fixture:dragon:1"}]
    }, ensure_ascii=False), encoding="utf-8")
    _, pixels = run("modded", 6560, opening, mods=mods)
    assert pixels > 100
    summoned, pixels = run("modded-summoned", 7500, SUMMON, OUT / "modded/end.state", mods=mods)
    assert pixels == 0
    assert struct.unpack_from("<3h", summoned, 0x1A7AE4) == (723, 4200, 3100)
    print(f"fusion helper: live stock/modded summons, selection, visibility and unchanged RNG/cards passed; {OUT}")


if __name__ == "__main__":
    main()
