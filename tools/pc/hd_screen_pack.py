#!/usr/bin/env python3
"""Make a screen's HD texture pack from your own disc, by a recipe.

A recipe (tools/pc/hd_recipes/*.json) names the sheet readings a screen
draws, as extract_images.py writes them (archive, offset, depth, palette),
the pieces a capture of the screen saw the game cut from each (pixel
rectangles of the reading), and how each reading is enlarged:

  model   painted art (a card frame, stone, a textured panel): Real-ESRGAN,
          mixed with a plain Lanczos enlargement (--model-share) so it is
          sharper without the grain the model invents. The whole reading
          goes to the model at once, every piece with its real neighbours;
          a piece the game cuts out on its own (cut_stands_out, from
          upscale_pack.py) is then redone alone, so its edge rows are not
          its sheet neighbours'.
  pixel   few-colour UI art (icons, symbols, boxes, labels, digits): xBR
          (ffmpeg's xbr filter), which keeps every shape and only rounds
          the steps. Each opaque region of the reading is done alone.
  tile    a picture the game repeats (a background tile): the model on
          each piece, wrapped around at its edges so the repeat stays
          seamless.

Every result is then pulled back to the original: each 4x4 block's average
is made the texel it came from (a few rounds of back-projection), so colours
and shading stay the game's and only the detail is new. Transparency keeps
the texels' own edges, except in regions no bigger than 40 pixels (icons,
arrows), whose outline is smoothed with xBR too. Only the regions a piece
of the recipe touches change; the rest of a reading is its texels, four
times, which draws exactly like the original.

Usage: hd_screen_pack.py tools/pc/hd_recipes/build_deck.json
                         [--data game/DATA] [--out <mods>/<id>]
                         [--upscaler realesrgan-ncnn-vulkan] [--model realesrgan-x4plus]
                         [--model-share 0.6]

Needs ffmpeg and an upscaler with the realesrgan-ncnn-vulkan command line
(realesrgan-ncnn-vulkan itself, or Upscayl's upscayl-bin with its models).
The pack holds images made from the game's, so it is for your own use:
make it from your disc, never commit or share the images (notes/image-remaster.md).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_images  # noqa: E402
from upscale_pack import cut_stands_out  # noqa: E402

S = 4                # the enlargement
PAD = 8              # texels of context around what a scaler sees
SMOOTH_MAX = 40      # regions up to this size get a smoothed outline
USUAL_UPSCALERS = [
    "realesrgan-ncnn-vulkan",
    "/opt/Upscayl/resources/bin/upscayl-bin",
    "/usr/lib/upscayl/resources/bin/upscayl-bin",
    r"C:\Program Files\Upscayl\resources\bin\upscayl-bin.exe",
    os.path.expandvars(r"%LOCALAPPDATA%\Programs\upscayl\resources\bin\upscayl-bin.exe"),
]


def find_upscaler(named):
    """The executable and its models directory."""
    for path in [named] if named else USUAL_UPSCALERS:
        found = shutil.which(path) if path else None
        if not found:
            continue
        found = os.path.realpath(found)
        for models in (os.path.join(os.path.dirname(os.path.dirname(found)), "models"),
                       os.path.join(os.path.dirname(found), "models"),
                       "/usr/share/realesrgan-ncnn-vulkan/models"):
            if os.path.isdir(models):
                return found, models
    sys.exit("no upscaler: install realesrgan-ncnn-vulkan (or Upscayl) or pass --upscaler <path>")


class Scaler:
    def __init__(self, upscaler, models, model, share, work):
        self.upscaler, self.models, self.model, self.share, self.work = upscaler, models, model, share, work
        self.runs = 0

    def pad(self, rgb, mode):
        return np.pad(rgb, ((PAD, PAD), (PAD, PAD), (0, 0)), mode=mode)

    def unpad(self, big):
        return big[PAD * S:-PAD * S, PAD * S:-PAD * S]

    def xbr(self, rgb):
        return self.unpad(self.ffmpeg([self.pad(rgb, "symmetric")], "xbr=4")[0])

    def ffmpeg(self, images, filt):
        out = []
        for i, image in enumerate(images):
            src, dst = os.path.join(self.work, f"x{i}.png"), os.path.join(self.work, f"x{i}-4.png")
            Image.fromarray(image).save(src)
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-vf", filt, dst], check=True)
            out.append(np.array(Image.open(dst).convert("RGB")))
        return out

    def models_run(self, jobs):
        """jobs: [(rgb, pad mode)] -> the model's 4x of each, mixed with Lanczos,
        in one run of the upscaler over a directory."""
        if not jobs:
            return []
        src, dst = os.path.join(self.work, "in"), os.path.join(self.work, "out")
        for d in (src, dst):
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
        padded = [self.pad(rgb, mode) for rgb, mode in jobs]
        for i, image in enumerate(padded):
            Image.fromarray(image).save(os.path.join(src, f"{i:05d}.png"))
        result = subprocess.run([self.upscaler, "-i", src, "-o", dst, "-m", self.models, "-n", self.model,
                                 "-s", "4", "-f", "png"], capture_output=True)
        self.runs += 1
        if result.returncode != 0:
            sys.exit(f"{self.upscaler} failed:\n{result.stderr.decode('utf-8', 'replace')[-2000:]}")
        out = []
        for i, image in enumerate(padded):
            model = np.array(Image.open(os.path.join(dst, f"{i:05d}.png")).convert("RGB"), dtype=np.float64)
            lanczos = np.array(Image.fromarray(image).resize((image.shape[1] * S, image.shape[0] * S), Image.LANCZOS),
                               dtype=np.float64)
            mixed = np.clip(model * self.share + lanczos * (1 - self.share), 0, 255).round().astype(np.uint8)
            out.append(self.unpad(mixed))
        return out


def fill_transparent(rgb, mask):
    """Transparent texels take the colour of the nearest opaque ones, so a
    scaler sees no black or magenta round a shape."""
    rgb, known = rgb.astype(np.float64), mask.copy()
    while not known.all() and known.any():
        total, count = np.zeros_like(rgb), np.zeros(mask.shape)
        for shift in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            k = np.roll(known, shift, (0, 1))
            total += np.roll(rgb, shift, (0, 1)) * k[..., None]
            count += k
        new = ~known & (count > 0)
        rgb[new] = total[new] / count[new][:, None]
        known |= new
    return rgb.round().astype(np.uint8)


def back_project(big, rgb, mask, rounds=6):
    """Nudge the enlargement until each block's average is its texel (the
    opaque ones): half spread bilinearly, half over the block."""
    out, src = big.astype(np.float64), rgb.astype(np.float64)
    h, w = mask.shape
    for _ in range(rounds):
        error = (src - out.reshape(h, S, w, S, 3).mean((1, 3))) * mask[..., None]
        spread = np.array(Image.fromarray(np.clip(error + 128, 0, 255).astype(np.uint8))
                          .resize((w * S, h * S), Image.BILINEAR), dtype=np.float64) - 128
        out += 0.5 * spread + 0.5 * np.repeat(np.repeat(error, S, 0), S, 1)
    return np.clip(out, 0, 255).round().astype(np.uint8)


def regions(mask):
    """8-connected opaque regions: a label per texel and their count."""
    labels = np.zeros(mask.shape, np.int32)
    count = 0
    for y, x in zip(*np.nonzero(mask)):
        if labels[y, x]:
            continue
        count += 1
        labels[y, x] = count
        stack = [(y, x)]
        while stack:
            cy, cx = stack.pop()
            for ny in range(max(cy - 1, 0), min(cy + 2, mask.shape[0])):
                for nx in range(max(cx - 1, 0), min(cx + 2, mask.shape[1])):
                    if mask[ny, nx] and not labels[ny, nx]:
                        labels[ny, nx] = count
                        stack.append((ny, nx))
    return labels, count


def blocks(a):
    return np.repeat(np.repeat(a, S, 0), S, 1)


class Reading:
    """One sheet reading and what the recipe does to it."""

    def __init__(self, path, sheet):
        image = np.array(Image.open(path).convert("RGBA"))
        self.rgb, self.mask = image[..., :3], image[..., 3] >= 128
        self.out = blocks(image).copy()
        self.method = sheet["method"]
        self.rects = sorted({tuple(r) for r in sheet["rects"]}, key=lambda r: -r[2] * r[3])
        used = np.zeros(self.mask.shape, bool)
        for x, y, w, h in self.rects:
            used[y:y + h, x:x + w] = True
        self.used = used

    def pieces(self):
        """(y0, x0, region mask) of every opaque region the recipe touches;
        for `model`, all of them as one."""
        labels, count = regions(self.mask)
        if self.method == "model":
            labels, count = np.where(self.mask, 1, 0), 1
        for label in range(1, count + 1):
            region = labels == label
            if not (region & self.used).any():
                continue
            ys, xs = np.nonzero(region)
            y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
            yield y0, x0, region[y0:y1, x0:x1]

    def lay(self, y0, x0, region, big, rgb):
        """Put a region's enlargement in, over its own texels and the
        transparent ones round it, never another region's."""
        h, w = region.shape
        big = back_project(big, rgb, region)
        own = blocks(region | ~self.mask[y0:y0 + h, x0:x0 + w])
        if region.all():
            alpha = np.ones(own.shape, bool)
        elif max(h, w) <= SMOOTH_MAX:
            alpha = xbr_alpha(region)
        else:
            alpha = blocks(region)
        tile = self.out[y0 * S:(y0 + h) * S, x0 * S:(x0 + w) * S]
        tile[own, :3] = big[own]
        tile[own, 3] = np.where(alpha[own], 255, 0)


def xbr_alpha(region):
    grey = np.repeat((region * 255).astype(np.uint8)[..., None], 3, 2)
    return SCALER.xbr(grey)[..., 0] >= 128


SCALER = None


def build(readings):
    """Every reading's pieces through its method, the model's in two batched runs."""
    first, jobs = [], []
    for reading in readings:
        if reading.method == "tile":
            for x, y, w, h in reading.rects:
                region = reading.mask[y:y + h, x:x + w]
                rgb = fill_transparent(reading.rgb[y:y + h, x:x + w], region)
                first.append((reading, y, x, region, rgb, True))
                jobs.append((rgb, "wrap"))
            continue
        for y0, x0, region in reading.pieces():
            h, w = region.shape
            rgb = fill_transparent(reading.rgb[y0:y0 + h, x0:x0 + w], region)
            if reading.method == "pixel":
                reading.lay(y0, x0, region, SCALER.xbr(rgb), rgb)
            else:
                first.append((reading, y0, x0, region, rgb, False))
                jobs.append((rgb, "symmetric"))
    for (reading, y0, x0, region, rgb, tile), big in zip(first, SCALER.models_run(jobs)):
        if tile:
            h, w = region.shape
            reading.out[y0 * S:(y0 + h) * S, x0 * S:(x0 + w) * S, :3] = back_project(big, rgb, region)
        else:
            reading.lay(y0, x0, region, big, rgb)
    # the pieces a model reading's game cuts out on their own, redone alone
    cuts, jobs = [], []
    for reading in readings:
        if reading.method != "model":
            continue
        sheet = Image.fromarray(np.dstack([reading.rgb, reading.mask * 255]).astype(np.uint8))
        for x, y, w, h in reading.rects:
            region = reading.mask[y:y + h, x:x + w]
            if w < 2 or h < 2 or not region.any() or not cut_stands_out(sheet, (x, y, w, h)):
                continue
            rgb = fill_transparent(reading.rgb[y:y + h, x:x + w], region)
            cuts.append((reading, x, y, w, h, region, rgb))
            jobs.append((rgb, "edge"))
    for (reading, x, y, w, h, region, rgb), big in zip(cuts, SCALER.models_run(jobs)):
        reading.out[y * S:(y + h) * S, x * S:(x + w) * S, :3] = back_project(big, rgb, region)
    return len(cuts)


def main():
    global SCALER
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("recipe")
    parser.add_argument("--data", default="game/DATA")
    parser.add_argument("--out", help="the mod folder to write (default: tmp/pc/packs/<id>)")
    parser.add_argument("--upscaler")
    parser.add_argument("--model", default="realesrgan-x4plus")
    parser.add_argument("--model-share", type=float, default=0.6)
    args = parser.parse_args()
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found: it makes the xBR enlargements")
    with open(args.recipe, encoding="utf-8") as handle:
        recipe = json.load(handle)
    out = args.out or os.path.join("tmp", "pc", "packs", recipe["id"])
    upscaler, models = find_upscaler(args.upscaler)
    with tempfile.TemporaryDirectory() as work:
        SCALER = Scaler(upscaler, models, args.model, args.model_share, work)
        images = os.path.join(work, "images")
        extractor = extract_images.Extractor(args.data, images)
        extractor.sheets_family()
        wanted = [(sheet["archive"], int(sheet["offset"], 16)) for sheet in recipe["sheets"]]
        extractor.sheets = [s for s in extractor.sheets if any(s.covers(a, o) for a, o in wanted)]
        extractor.emit_sheets()
        by_identity = {}
        for entry in extractor.manifest:
            by_identity.setdefault((entry["archive"], entry["offset"], entry["bpp"], entry.get("clut_offset")), entry)
        readings, entries = [], []
        for sheet in recipe["sheets"]:
            key = (sheet["archive"], int(sheet["offset"], 16), sheet["bpp"], int(sheet["palette"], 16))
            entry = by_identity.get(key)
            if entry is None:
                sys.exit(f"no sheet reading {key} ({sheet['what']}): does extract_images.py read it?")
            reading = Reading(os.path.join(images, entry["file"]), sheet)
            reading.name = entry["file"].replace("sheets/", "").replace("/", "-")
            readings.append(reading)
            entries.append(dict(entry, file=reading.name, alias=f"{recipe['name']}: {sheet['what']}"))
        print(f"{len(readings)} readings: model {upscaler} ({args.model}), xBR through ffmpeg")
        cuts = build(readings)
        if os.path.isdir(out):
            shutil.rmtree(out)
        textures = os.path.join(out, "textures")
        os.makedirs(textures)
        for reading in readings:
            Image.fromarray(reading.out).save(os.path.join(textures, reading.name))
        with open(os.path.join(textures, "manifest.json"), "w", encoding="utf-8") as handle:
            json.dump(entries, handle, indent=1)
        manifest = {key: recipe[key] for key in ("id", "name", "version", "author", "description")}
        manifest.update(enabled=True, textures="textures")
        with open(os.path.join(out, "mod.json"), "w", encoding="utf-8") as handle:
            json.dump(manifest, handle, indent=4)
        print(f"{out}: {len(readings)} images ({cuts} pieces redone alone, {SCALER.runs} upscaler runs)")


if __name__ == "__main__":
    main()
