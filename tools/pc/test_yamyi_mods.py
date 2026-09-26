#!/usr/bin/env python3
"""Live Yamyi Mods checks; needs a built game and the player's disc.
Uses isolated settings/saves and a fixture that opens the mode wheel with
all Library cards visible. Run with --windows to exercise the same mod in Wine.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tmp/pc/yamyi-live-test"
FIXTURE = """#include "pc/mods/modapi.h"
static void *original;
void MainMenu_InitFrontendMenu(int unused, int menu);
static void init_menu(int unused, int menu) {
    unsigned char *chest = (unsigned char *)0x801D0250;
    int i;
    (void)menu;
    *(unsigned char *)0x8009B26C = 8;
    for (i = 0; i < 722; i++) chest[i] = 1;
    ((void (*)(int,int))original)(unused,5);
}
int MemoriesModInit(const MemoriesModHost *h, MemoriesMod *m) {
    m->api=4;
    return h->hook(h,MainMenu_InitFrontendMenu,init_menu,&original)!=0;
}
"""


def memory(path):
    data = path.read_bytes()
    at = 16
    while at + 20 <= len(data):
        tag = data[at:at + 16].split(b"\0")[0]
        size, = struct.unpack_from("<I", data, at + 16)
        if tag == b"memory":
            return data[at + 20:at + 20 + size]
        at += 20 + size
    raise AssertionError("Missing guest memory")


def pixels(path, rgb):
    data = path.read_bytes()
    offset, = struct.unpack_from("<I", data, 10)
    width, height = struct.unpack_from("<ii", data, 18)
    bits, = struct.unpack_from("<H", data, 28)
    stride = ((width * bits + 31) // 32) * 4
    # Menu text is antialiased: identify the score colour by its RGB ratios.
    result = 0
    for y in range(abs(height)):
        for x in range(width):
            at = offset + y * stride + x * (bits // 8)
            blue, green, red = data[at:at + 3]
            if blue > 100 and abs(red * 255 - blue * (rgb >> 16)) < 255 and abs(green * 255 - blue * ((rgb >> 8) & 255)) < 255:
                result += 1
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--windows", action="store_true")
    parser.add_argument("--skip-pixel-checks", action="store_true",
                        help="Check game state only when Wine cannot capture its window")
    options = parser.parse_args()
    build = ROOT / ("tmp/pc/win32" if options.windows else "tmp/pc/game32")
    mods = OUT / "mods"
    fixture = mods / "fixture"
    fixture.mkdir(parents=True, exist_ok=True)
    (fixture / "mod.json").write_text(json.dumps(dict(id="fixture", name="Screen fixture",
        min_api=4, enabled=True, library="fixture")))
    (fixture / "fixture.c").write_text(FIXTURE)
    subprocess.run(["python3", str(ROOT / "tools/pc/build_mod.py"), str(fixture)], check=True)
    shutil.copytree(build / "mods/yamyi-mods", mods / "yamyi-mods", dirs_exist_ok=True)

    def run(name, sequence, frame, settings="", state=None):
        folder = OUT / (name + ("-windows" if options.windows else "-linux"))
        folder.mkdir(exist_ok=True)
        (folder / "settings").write_text("mod.yamyi-mods=1\n" + settings)
        env = {k: v for k, v in os.environ.items() if not k.startswith("MEMORIES_")}
        env.update(SDL_VIDEODRIVER="offscreen", MEMORIES_DETERMINISTIC="1",
            MEMORIES_NO_AUDIO="1", MEMORIES_NO_GAMEPAD="1", MEMORIES_SPEED="-1",
            MEMORIES_SETTINGS=str(folder / "settings"), MEMORIES_USER_DIR=str(folder / "user"),
            MEMORIES_MODS_DIR=str(mods), MEMORIES_INPUT=sequence,
            MEMORIES_WINDOW_SHOT=str(frame), MEMORIES_DUMP_FRAME=str(frame + 1),
            MEMORIES_SCREENSHOT_DIR=str(folder), MEMORIES_DUMP_PATH=str(folder / "game.ppm"),
            MEMORIES_SAVE_STATE=f"{frame}:{folder / 'end.state'}")
        if "MEMORIES_DISC" in os.environ: env["MEMORIES_DISC"] = os.environ["MEMORIES_DISC"]
        if state: env["MEMORIES_LOAD_STATE"] = str(state)
        command = [str(build / "memories-pc")]
        if options.windows:
            env["SDL_VIDEODRIVER"] = "windows"
            command = ["wine", str(build / "memories-pc.exe")]
            env.update(WINEPREFIX=str(ROOT / "tmp/pc/wine-prefix"),
                       WINEDLLOVERRIDES="mscoree,mshtml=", WINEDEBUG="-all")
        with (folder / "run.log").open("w") as log:
            subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=120)
        shot = max(folder.glob("*.bmp"), key=lambda p: p.stat().st_mtime_ns)
        return folder / "end.state", shot

    circle = "1140:2000,1146:0000"
    prompt, _ = run("prompt", circle, 1220)
    assert memory(prompt)[0x184594] == 5
    no, _ = run("no", circle + ",1250:4000,1256:0000", 1350)
    assert memory(no)[0x184594] == 5
    yes, _ = run("yes", circle + ",1240:0080,1246:0000,1280:4000,1286:0000", 1400)
    assert memory(yes)[0x184594] == 1
    disabled, _ = run("disabled", circle, 1300, "mod.yamyi-mods.confirm_exit=0\n")
    assert memory(disabled)[0x184594] == 1
    library = "1100:0040,1106:0000,1150:0040,1156:0000,1200:0040,1206:0000,1300:4000,1306:0000"
    saved, shot = run("library", library, 1650)
    assert memory(saved)[0x9B26C] & 0x5F == 0x44
    if not options.skip_pixel_checks: assert pixels(shot, 0x7FC9FF) > 0, "Missing score column"
    _, shot = run("no-score", library, 1650, "mod.yamyi-mods.show_score=0\n")
    if not options.skip_pixel_checks: assert pixels(shot, 0x7FC9FF) == 0
    _, shot = run("no-panel", library, 1650, "mod.yamyi-mods.drops=0\n")
    if not options.skip_pixel_checks: assert pixels(shot, 0x7FC9FF) == 0
    loaded, shot = run("reload", "", 1700, state=saved)
    assert memory(loaded)[0x9B26C] & 0x5F == 0x44
    if not options.skip_pixel_checks: assert pixels(shot, 0x7FC9FF) > 0
    print("Yamyi Mods: prompt NO/YES/bypass, Library and save/load passed; " +
          ("pixel checks skipped" if options.skip_pixel_checks else "pixel checks passed"))


if __name__ == "__main__":
    main()
