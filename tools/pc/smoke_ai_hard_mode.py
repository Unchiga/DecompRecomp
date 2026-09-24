#!/usr/bin/env python3
"""Exercise AI turns and upcoming-card swaps in the packaged game (requires disc data)."""
import argparse
import json
import os
from pathlib import Path
import random
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("linux", "windows", "both"), default="linux")
    parser.add_argument("--retail-tactics", action="store_true", help="Exercise original scripts with information/swap overrides")
    args = parser.parse_args()
    base = json.loads((ROOT / "tests/pc/smoke/duel-hand-camera.json").read_text())["input"]
    sequence = [base]
    rng = random.Random(314)
    for frame in range(6600, 16000, 30):
        key = rng.choice([0x4000, 0x4000, 0x2000, 0x8000, 0x10, 0x20, 0x40, 0x80, 0x800, 0x8])
        sequence.extend((f"{frame}:{key:04x}", f"{frame + 6}:0000"))
    targets = ("linux", "windows") if args.target == "both" else (args.target,)
    for target in targets:
        out = ROOT / "tmp/pc/ai-hard-mode-smoke" / (target + ("-retail" if args.retail_tactics else ""))
        out.mkdir(parents=True, exist_ok=True)
        settings = out / "settings.txt"
        settings.write_text("mod.ai-hard-mode=1\nmod.ai-hard-mode.trace=1\n"
                            "mod.ai-hard-mode.deck_access=2\nmod.ai-hard-mode.hidden_cards=1\n"
                            "mod.3d-monsters=0\nmod.hand-camera=0\n")
        if args.retail_tactics:
            with settings.open("a") as file:
                for key in ("hand_planning", "attack_planning", "defensive_positions", "spell_timing"):
                    file.write(f"mod.ai-hard-mode.{key}=0\n")
        log = out / "trace.log"
        log.unlink(missing_ok=True)
        environment = dict(os.environ, MEMORIES_HEADLESS="1", MEMORIES_NO_AUDIO="1",
                           MEMORIES_NO_GAMEPAD="1", MEMORIES_SPEED="-1", MEMORIES_WATCHDOG="30",
                           MEMORIES_SETTINGS=str(settings), MEMORIES_USER_DIR=str(out / "user"),
                           MEMORIES_INPUT=",".join(sequence), MEMORIES_DUMP_FRAME="16000",
                           MEMORIES_DUMP_PATH=str(out / "frame.ppm"),
                           MEMORIES_TRACE="mods", MEMORIES_LOG=str(log))
        program = ROOT / ("tmp/pc/game32/memories-pc" if target == "linux" else "tmp/pc/win32/memories-pc.exe")
        launch = [str(program)]
        if target == "windows" and sys.platform != "win32":
            launch.insert(0, "wine")
            environment.update(WINEPREFIX=str(ROOT / "tmp/pc/wine-prefix"),
                               WINEDLLOVERRIDES="mscoree,mshtml=", WINEDEBUG="-all")
        with (out / "stdout.log").open("w") as output:
            subprocess.run(launch, cwd=ROOT, env=environment, stdout=output,
                           stderr=subprocess.STDOUT, check=True, timeout=120)
        decisions = re.findall(r"ai-hard-mode: opponent=(\d+) phase=(hand|field) result=(\d+) card=(\d+)", log.read_text())
        assert len(decisions) >= 6, f"AI did not advance: inspect {out}"
        assert sum(phase == "field" and result == "3" for _, phase, result, _ in decisions) >= 2
        if not args.retail_tactics:
            assert any(phase == "hand" and int(card) >= 16 for _, phase, _, card in decisions), "No upcoming-card selection exercised"
        assert (out / "frame.ppm").is_file()
        print(f"AI hard mode: {target} {'retail fallback' if args.retail_tactics else 'planner'} runtime passed ({len(decisions)} decisions and completed turns)", flush=True)


if __name__ == "__main__":
    main()
