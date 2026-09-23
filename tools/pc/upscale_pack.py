#!/usr/bin/env python3
"""Upscale a set of extracted images into a texture pack with Upscayl.

Takes what extract_images.py wrote (PNGs and their manifest.json) and makes
a mod folder whose `textures` directory holds the same images upscaled, the
manifest carried over unchanged, so the pack matches the game's uploads by
origin (notes/modding.md, "Texture packs"). The images are made by Upscayl's
command-line binary (Real-ESRGAN on the GPU through Vulkan), the way the
Upscayl window makes them: the model's own 4x per pass, resized to the
scale asked for.

Usage: upscale_pack.py --images tmp/pc/images --images tmp/pc/images-story
                       --out mods/hd-all [--merge mods/hd-portraits]
                       [--scale 4] [--passes N] [--max-side 2048]
                       [--model high-fidelity-4x] [--min-size 32]
                       [--only assets/] [--zip hd-story.zip] [--upscayl PATH]

--images may be given several times: one pack from all the sets (an image
in two sets, the same path, is taken once). --merge adds an existing pack's
images as they are, already upscaled some other way.
--scale is the whole enlargement (4: a 128x128 background becomes 512x512,
what the game's Internal 4x shows one to one). It takes ceil(log4 scale)
passes unless --passes says otherwise; each pass runs the model's 4x and
resizes to the pass's share (--scale 25 --passes 2 is the Upscayl window's
"5x, twice": 48x48 to 1200x1200). --max-side lowers the scale of an image
whose result would be bigger: the game holds every image of a pack in
memory, at its full size. --only keeps the manifest entries whose file or
alias contains the text; --min-size skips images with a side below it
(icons, glyphs), which the model does not improve and whose overlapping
sub-rectangles (a font's glyphs cut from one sheet) the pack cannot tell
apart yet. An output that exists
already is kept, so a run can be resumed or extended. Upscayl is looked for
at its usual install path; --upscayl names the upscayl-bin executable (its
models are beside it). --zip also writes the pack as a zip with the mod
folder inside, ready to unpack into a mods directory.
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

from PIL import Image

USUAL_UPSCAYL = [
    r"C:\Program Files\Upscayl\resources\bin\upscayl-bin.exe",
    os.path.expandvars(r"%LOCALAPPDATA%\Programs\upscayl\resources\bin\upscayl-bin.exe"),
    "/opt/Upscayl/resources/bin/upscayl-bin",
    "/usr/lib/upscayl/resources/bin/upscayl-bin",
]


def find_upscayl(named: str | None) -> tuple[str, str]:
    """The executable and its models directory."""
    candidates = [named] if named else USUAL_UPSCAYL
    for path in candidates:
        if path and os.path.isfile(path):
            models = os.path.join(os.path.dirname(os.path.dirname(path)), "models")
            if not os.path.isdir(models):
                models = os.path.join(os.path.dirname(path), "models")
            return path, models
    sys.exit("upscayl-bin not found; install Upscayl or pass --upscayl <path to upscayl-bin>")


def run_model(upscayl: str, models: str, model: str, source: str, target: str) -> None:
    """The model's 4x over a directory of images."""
    os.makedirs(target, exist_ok=True)
    command = [upscayl, "-i", source, "-o", target, "-m", models, "-n", model, "-s", "4", "-f", "png"]
    result = subprocess.run(command, capture_output=True)  # bytes: its messages are UTF-8 whatever the console's page
    if result.returncode != 0:
        sys.exit(f"upscayl-bin failed on {source}:\n{result.stderr.decode('utf-8', 'replace')[-2000:]}")


def resize_to(path: str, width: int, height: int) -> None:
    with Image.open(path) as image:
        if image.size == (width, height):
            return
        resized = image.resize((width, height), Image.LANCZOS)
    resized.save(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--images", action="append", default=[],
                        help="directory with manifest.json from extract_images.py; repeatable, one pack from all")
    parser.add_argument("--merge", action="append", default=[],
                        help="an existing pack folder whose images join the pack as they are (already upscaled)")
    parser.add_argument("--out", required=True, help="the mod folder to make (mod.json, images/)")
    parser.add_argument("--model", default="high-fidelity-4x")
    parser.add_argument("--scale", type=float, default=4, help="the whole enlargement")
    parser.add_argument("--passes", type=int, help="model passes (default: as many 4x as the scale needs)")
    parser.add_argument("--max-side", type=int, default=2048, help="an image is scaled less than asked if bigger")
    parser.add_argument("--min-size", type=int, default=32, help="skip images with a side below this")
    parser.add_argument("--only", action="append", default=[], help="keep entries whose file or alias has this")
    parser.add_argument("--upscayl", help="path to upscayl-bin")
    parser.add_argument("--name", help="the mod's name shown in the Mods window")
    parser.add_argument("--zip", help="also write the pack as this zip, the mod folder inside it, to hand out")
    args = parser.parse_args()
    if args.scale < 1:
        sys.exit("--scale is at least 1")

    upscayl, models = find_upscayl(args.upscayl)
    if not os.path.isfile(os.path.join(models, args.model + ".bin")):
        sys.exit(f"no model {args.model} in {models}; there: " +
                 ", ".join(sorted(n[:-4] for n in os.listdir(models) if n.endswith(".bin"))))
    if not args.images and not args.merge:
        sys.exit("--images or --merge is needed")
    manifest = []
    seen = set()
    for images in args.images:
        with open(os.path.join(images, "manifest.json"), encoding="utf-8") as handle:
            for entry in json.load(handle):
                if entry["file"] in seen:
                    continue  # the same image (its path names its origin) from two sets
                seen.add(entry["file"])
                entry["source"] = images
                manifest.append(entry)
    chosen = []
    for entry in manifest:
        if entry["width"] < args.min_size or entry["height"] < args.min_size:
            continue
        if args.only and not any(text in entry["file"] or text in entry.get("alias", "") for text in args.only):
            continue
        chosen.append(entry)
    if not chosen and not args.merge:
        sys.exit("no image chosen")

    images_out = os.path.join(args.out, "images")
    os.makedirs(images_out, exist_ok=True)
    pending = [e for e in chosen if not os.path.isfile(os.path.join(images_out, e["file"]))]
    passes = args.passes or max(1, math.ceil(math.log(args.scale, 4) - 1e-9))
    print(f"{len(chosen)} images chosen of {len(manifest)}, {len(pending)} to make "
          f"({args.model}, {args.scale:g}x in {passes} pass{'es' if passes > 1 else ''})")
    if pending:
        work = tempfile.mkdtemp(prefix="upscale-")
        try:
            # Flat names in the work directories: the manifest's paths have folders.
            flat = {f"{index:05d}.png": entry for index, entry in enumerate(pending)}
            stage = os.path.join(work, "in")
            os.makedirs(stage)
            for name, entry in flat.items():
                shutil.copyfile(os.path.join(entry["source"], entry["file"]), os.path.join(stage, name))
            for number in range(1, passes + 1):
                target = os.path.join(work, f"pass{number}")
                print(f"pass {number}: {len(flat)} images...", flush=True)
                run_model(upscayl, models, args.model, stage, target)
                for name, entry in flat.items():
                    # This pass's share of the scale, from the original's size; the
                    # last pass lands exactly on the scale, capped by --max-side.
                    scale = args.scale
                    longest = max(entry["width"], entry["height"])
                    if longest * scale > args.max_side:
                        scale = args.max_side / longest
                    share = scale ** (number / passes)
                    resize_to(os.path.join(target, name), max(1, round(entry["width"] * share)),
                              max(1, round(entry["height"] * share)))
                stage = target
            for name, entry in flat.items():
                destination = os.path.join(images_out, entry["file"])
                os.makedirs(os.path.dirname(destination), exist_ok=True)
                shutil.move(os.path.join(stage, name), destination)
        finally:
            shutil.rmtree(work, ignore_errors=True)
    seen = {entry["file"] for entry in chosen}
    for pack in args.merge:
        # Another pack's images as they are, its manifest entries with them.
        with open(os.path.join(pack, "images", "manifest.json"), encoding="utf-8") as handle:
            for entry in json.load(handle):
                if entry["file"] in seen:
                    continue
                seen.add(entry["file"])
                destination = os.path.join(images_out, entry["file"])
                if not os.path.isfile(destination):
                    os.makedirs(os.path.dirname(destination), exist_ok=True)
                    shutil.copyfile(os.path.join(pack, "images", entry["file"]), destination)
                chosen.append(entry)
    # A pack built in several runs (--only cards/ at one --min-size, then
    # --only assets/ at another): what an earlier run put there stays.
    existing = os.path.join(images_out, "manifest.json")
    if os.path.isfile(existing):
        with open(existing, encoding="utf-8") as handle:
            for entry in json.load(handle):
                if entry["file"] not in seen and os.path.isfile(os.path.join(images_out, entry["file"])):
                    seen.add(entry["file"])
                    chosen.append(entry)
    for entry in chosen:
        entry.pop("source", None)
    with open(os.path.join(images_out, "manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(chosen, handle, indent=1)
    mod_json = os.path.join(args.out, "mod.json")
    if not os.path.isfile(mod_json):
        identity = os.path.basename(os.path.normpath(args.out))
        with open(mod_json, "w", encoding="utf-8") as handle:
            json.dump({"id": identity, "name": args.name or identity, "textures": "images", "enabled": False},
                      handle, indent=1)
    print(f"pack: {args.out} ({len(chosen)} images)")
    if args.zip:
        # The folder itself is in the zip, so it unpacks into a mods directory as one mod.
        folder = os.path.basename(os.path.normpath(args.out))
        with zipfile.ZipFile(args.zip, "w", zipfile.ZIP_DEFLATED) as archive:
            for root, _, names in os.walk(args.out):
                for name in sorted(names):
                    path = os.path.join(root, name)
                    archive.write(path, os.path.join(folder, os.path.relpath(path, args.out)))
        print(f"zip: {args.zip} ({os.path.getsize(args.zip) // (1 << 20)} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
