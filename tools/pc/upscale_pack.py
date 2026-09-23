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
A sheet's columns (the `sheets` family marks them) are joined side by side
for the model where one picture runs on from one into the next, as they
stand in VRAM, and cut apart again: no seam, and no bleeding from a column
that holds something else.
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

from PIL import Image, ImageChops, ImageStat

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


def continuous(left: dict, right: dict) -> bool:
    """Whether one picture runs on from the left column into the right one:
    the step across the join is no bigger than the steps just inside each
    column. A column that holds something else (a mask, another sprite)
    would bleed into its neighbour's edge under the model."""
    def step(a: Image.Image, b: Image.Image) -> float:
        return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 4

    with Image.open(os.path.join(left["source"], left["file"])) as l_image, \
            Image.open(os.path.join(right["source"], right["file"])) as r_image:
        l_image, r_image = l_image.convert("RGBA"), r_image.convert("RGBA")
        h = min(l_image.height, r_image.height)
        l_edge, l_inner = l_image.crop((l_image.width - 1, 0, l_image.width, h)), l_image.crop((l_image.width - 2, 0, l_image.width - 1, h))
        r_edge, r_inner = r_image.crop((0, 0, 1, h)), r_image.crop((1, 0, 2, h))
        across = step(l_edge, r_edge)
        inside = max(step(l_inner, l_edge), step(r_edge, r_inner))
        return across <= inside * 2 + 8


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
    def identity(entry: dict) -> tuple:
        """What the game matches an entry by: its words on the disc and how it reads them."""
        return (entry["archive"], entry["offset"], entry["words"], entry["rows"], entry["bpp"],
                entry.get("clut_offset"), entry.get("stride"), tuple(entry.get("row_offsets") or ()))

    manifest = []
    seen = set()
    for images in args.images:
        with open(os.path.join(images, "manifest.json"), encoding="utf-8") as handle:
            for entry in json.load(handle):
                if identity(entry) in seen:
                    continue  # the same reading of the same words from two sets
                seen.add(identity(entry))
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
    # Entries with identical pictures share one file (the extractor writes it once): one image job.
    files = {}
    for entry in chosen:
        files.setdefault(entry["file"], entry)
    pending = [e for f, e in files.items() if not os.path.isfile(os.path.join(images_out, f))]
    passes = args.passes or max(1, math.ceil(math.log(args.scale, 4) - 1e-9))
    print(f"{len(chosen)} images chosen of {len(manifest)} ({len(files)} files), {len(pending)} to make "
          f"({args.model}, {args.scale:g}x in {passes} pass{'es' if passes > 1 else ''})")
    if pending:
        work = tempfile.mkdtemp(prefix="upscale-")
        try:
            # Flat names in the work directories: the manifest's paths have
            # folders. A sheet's columns (the extractor marks them) stand side
            # by side in VRAM, so the model sees them joined into one picture
            # and nothing shows at the joins; the result is cut back apart.
            jobs = []  # [entries left to right]
            by_sheet = {}
            for entry in pending:
                if "sheet" in entry:
                    by_sheet.setdefault((entry["sheet"], entry["bpp"], entry.get("clut_offset"), entry["source"]),
                                        []).append(entry)
                else:
                    jobs.append([entry])
            for parts in by_sheet.values():
                parts.sort(key=lambda e: e["column"])
                run = []
                for entry in parts:
                    if run and (entry["column"] != run[-1]["column"] + 1 or entry["height"] != run[-1]["height"]
                                or not continuous(run[-1], entry)):
                        jobs.append(run)
                        run = []
                    run.append(entry)
                jobs.append(run)
            flat = {f"{index:05d}.png": parts for index, parts in enumerate(jobs)}
            stage = os.path.join(work, "in")
            os.makedirs(stage)
            for name, parts in flat.items():
                if len(parts) == 1:
                    shutil.copyfile(os.path.join(parts[0]["source"], parts[0]["file"]), os.path.join(stage, name))
                    continue
                joined = Image.new("RGBA", (sum(e["width"] for e in parts), parts[0]["height"]))
                x = 0
                for entry in parts:
                    with Image.open(os.path.join(entry["source"], entry["file"])) as image:
                        joined.paste(image.convert("RGBA"), (x, 0))
                    x += entry["width"]
                joined.save(os.path.join(stage, name))
            shares = {}
            for number in range(1, passes + 1):
                target = os.path.join(work, f"pass{number}")
                print(f"pass {number}: {len(flat)} images...", flush=True)
                run_model(upscayl, models, args.model, stage, target)
                for name, parts in flat.items():
                    # This pass's share of the scale, from the original's size; the
                    # last pass lands exactly on the scale, capped by --max-side
                    # (of one column, when several are joined).
                    scale = args.scale
                    longest = max(max(e["width"], e["height"]) for e in parts)
                    if longest * scale > args.max_side:
                        scale = args.max_side / longest
                    share = scale ** (number / passes)
                    shares[name] = share
                    resize_to(os.path.join(target, name), max(1, sum(max(1, round(e["width"] * share)) for e in parts)),
                              max(1, round(parts[0]["height"] * share)))
                stage = target
            for name, parts in flat.items():
                if len(parts) == 1:
                    destination = os.path.join(images_out, parts[0]["file"])
                    os.makedirs(os.path.dirname(destination), exist_ok=True)
                    shutil.move(os.path.join(stage, name), destination)
                    continue
                with Image.open(os.path.join(stage, name)) as joined:
                    x = 0
                    for entry in parts:
                        width = max(1, round(entry["width"] * shares[name]))
                        destination = os.path.join(images_out, entry["file"])
                        os.makedirs(os.path.dirname(destination), exist_ok=True)
                        joined.crop((x, 0, x + width, joined.height)).save(destination)
                        x += width
        finally:
            shutil.rmtree(work, ignore_errors=True)
    seen = {identity(entry) for entry in chosen}
    for pack in args.merge:
        # Another pack's images as they are, its manifest entries with them.
        with open(os.path.join(pack, "images", "manifest.json"), encoding="utf-8") as handle:
            for entry in json.load(handle):
                if identity(entry) in seen:
                    continue
                seen.add(identity(entry))
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
                if identity(entry) not in seen and os.path.isfile(os.path.join(images_out, entry["file"])):
                    seen.add(identity(entry))
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
