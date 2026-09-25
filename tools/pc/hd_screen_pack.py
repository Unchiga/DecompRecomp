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
  pixel   few-colour UI art (icons, symbols, boxes): xBR
          (ffmpeg's xbr filter), which keeps every shape and only rounds
          the steps. Each opaque region of the reading is done alone.
  tile    a picture the game repeats (a background tile): the model on
          each piece, wrapped around at its edges so the repeat stays
          seamless.

A sheet may add `alone` (every piece enlarged on its own, its edges carried
on: tiles, a box's repeated middle) and `pixel` (rectangles of a painted
reading done by xBR instead: an icon beside a frame). Readings of the same
pixels (one sheet in several packages) are made once. A recipe's `trim`
leaves what a reading never draws as its texels four times (small files);
its `part` is written into each entry as the HD mod's setting
(hd_assets_pack.py).

Every result is then pulled back to the original: each 4x4 block's average
is made the texel it came from (a few rounds of back-projection), so colours
and shading stay the game's and only the detail is new. Transparency keeps
the texels' own edges, except in regions no bigger than 40 pixels (icons,
arrows), whose outline is smoothed with xBR too. Only the regions a piece
of the recipe touches change; the rest of a reading is its texels, four
times, which draws exactly like the original.

A recipe's `labels` are text the game draws as pictures (CHEST, ORDER,
DECK, the small digits). No scaler makes clean letters of 7-texel ones, so
after the rest they are set anew in a bold sans (--font; by default the one
HD text uses), each fitted to the game's letters and drawn in the reading's
own palette entries: `shadow` style, a word with its shadow a texel down
and right (over clear texels when `background` is entry 0, with an `edge`
colour round the fill if given); `outline` style, one character (or word,
when `text` is a list) a cell inside a texel of outline;
`strip` style, a word the game cuts into pieces drawn side by side, set once
across them and anti-aliased from the background to the fill. A label names
every palette the game reads it with (and `offset` may list the packages),
and each of those readings gets it; `bpp` 8 reads an 8-bit sheet, `font` is
a fontconfig pattern of its own ("serif:bold"), and `clip` keeps an
outlined letter to the game's glyph texels (texels drawn both plain and
subtracted).

Usage: hd_screen_pack.py tools/pc/hd_recipes/build_deck.json
                         [--data game/DATA] [--out <mods>/<id>]
                         [--upscaler realesrgan-ncnn-vulkan] [--model realesrgan-x4plus]
                         [--model-share 0.6] [--font <bold .ttf>]

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
from PIL import Image, ImageDraw, ImageFont

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

    def xbr(self, rgb, mode="symmetric"):
        return self.unpad(self.ffmpeg([self.pad(rgb, mode)], "xbr=4")[0])

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
        for attempt in range(3):
            result = subprocess.run([self.upscaler, "-i", src, "-o", dst, "-m", self.models, "-n", self.model,
                                     "-s", "4", "-f", "png"], capture_output=True)
            self.runs += 1
            if result.returncode != 0:
                sys.exit(f"{self.upscaler} failed:\n{result.stderr.decode('utf-8', 'replace')[-2000:]}")
            # the upscaler has been seen to exit with an output not yet whole: run it again
            if all(readable(os.path.join(dst, f"{i:05d}.png")) for i in range(len(padded))):
                break
        else:
            bad = [i for i in range(len(padded)) if not readable(os.path.join(dst, f"{i:05d}.png"))]
            sys.exit(f"{self.upscaler} left {len(bad)} unreadable outputs in {dst} (first {bad[:5]})")
        out = []
        for i, image in enumerate(padded):
            model = np.array(Image.open(os.path.join(dst, f"{i:05d}.png")).convert("RGB"), dtype=np.float64)
            lanczos = np.array(Image.fromarray(image).resize((image.shape[1] * S, image.shape[0] * S), Image.LANCZOS),
                               dtype=np.float64)
            mixed = np.clip(model * self.share + lanczos * (1 - self.share), 0, 255).round().astype(np.uint8)
            out.append(self.unpad(mixed))
        return out


def readable(path):
    try:
        with Image.open(path) as image:
            image.load()
        return True
    except (OSError, ValueError):
        return False


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
        self.alone = sheet.get("alone", False)
        self.pixel = [tuple(r) for r in sheet.get("pixel", [])]
        self.rects = sorted({tuple(r) for r in sheet["rects"]}, key=lambda r: -r[2] * r[3])
        used = np.zeros(self.mask.shape, bool)
        for x, y, w, h in self.rects + self.pixel:
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
    """Every reading's pieces through its method, the model's in two batched runs.
    Readings of the same pixels by the same recipe (one sheet in several
    packages) are made once."""
    made, copies = {}, []
    for reading in readings:
        key = (reading.rgb.tobytes(), reading.mask.tobytes(), reading.method, reading.alone, tuple(reading.rects),
               tuple(map(tuple, reading.pixel)))
        if key in made:
            copies.append((reading, made[key]))
        else:
            made[key] = reading
    readings = list(made.values())
    cuts = build_unique(readings)
    for reading, same in copies:
        reading.out = same.out.copy()
    return cuts


def build_unique(readings):
    first, jobs = [], []
    for reading in readings:
        if reading.method == "tile":
            for x, y, w, h in reading.rects:
                region = reading.mask[y:y + h, x:x + w]
                rgb = fill_transparent(reading.rgb[y:y + h, x:x + w], region)
                first.append((reading, y, x, region, rgb, True))
                jobs.append((rgb, "wrap"))
            continue
        if reading.method == "pixel" and reading.alone:
            # pieces the game repeats side by side (a box's middle): each
            # alone, its edges carried on, so the repeats meet without a seam
            for x, y, w, h in reading.rects:
                region = reading.mask[y:y + h, x:x + w]
                if region.any():
                    rgb = fill_transparent(reading.rgb[y:y + h, x:x + w], region)
                    reading.lay(y, x, region, SCALER.xbr(rgb, "edge"), rgb)
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
            if w < 2 or h < 2 or not region.any():
                continue
            if not reading.alone and not cut_stands_out(sheet, (x, y, w, h)):
                continue
            rgb = fill_transparent(reading.rgb[y:y + h, x:x + w], region)
            cuts.append((reading, x, y, w, h, region, rgb))
            jobs.append((rgb, "edge"))
    for (reading, x, y, w, h, region, rgb), big in zip(cuts, SCALER.models_run(jobs)):
        reading.out[y * S:(y + h) * S, x * S:(x + w) * S, :3] = back_project(big, rgb, region)
    # few-colour pieces on a painted sheet (an icon beside a frame): xBR, each region alone
    for reading in readings:
        for x, y, w, h in reading.pixel:
            labels, count = regions(reading.mask[y:y + h, x:x + w])
            for label in range(1, count + 1):
                ys, xs = np.nonzero(labels == label)
                y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
                region = (labels == label)[y0:y1, x0:x1]
                rgb = fill_transparent(reading.rgb[y + y0:y + y1, x + x0:x + x1], region)
                reading.lay(y + y0, x + x0, region, SCALER.xbr(rgb), rgb)
    return len(cuts)


def default_font():
    """A bold sans like the one HD text draws with (glyphs.c asks fontconfig
    for sans-serif:bold; on Windows, Segoe UI Bold or Arial Bold)."""
    if os.name == "nt":
        fonts = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
        for name in ("segoeuib.ttf", "arialbd.ttf"):
            if os.path.isfile(os.path.join(fonts, name)):
                return os.path.join(fonts, name)
    elif shutil.which("fc-match"):
        found = subprocess.run(["fc-match", "-f", "%{file}", "sans-serif:bold"], capture_output=True, text=True)
        if found.returncode == 0 and os.path.isfile(found.stdout):
            return found.stdout
    sys.exit("no bold sans font found for the labels: pass --font <file.ttf>")


def text_cover(text, font_file, width, height):
    """The text's ink (0-1) stretched to exactly width x height pixels: set
    large, cropped to its ink and brought down, so the edges are smooth."""
    font = ImageFont.truetype(font_file, 200)
    left, top, right, bottom = font.getbbox(text)
    image = Image.new("L", (right - left + 8, bottom - top + 8))
    ImageDraw.Draw(image).text((4 - left, 4 - top), text, font=font, fill=255)
    image = image.crop(image.getbbox())
    if width is None:
        width = max(1, round(image.width * height / image.height))
    return np.array(image.resize((width, height), Image.LANCZOS), np.float64) / 255


def grow(cover, radius):
    """The cover spread by `radius` pixels round, for an outline."""
    out = cover.copy()
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            if dx * dx + dy * dy > radius * radius + radius:
                continue
            shifted = np.roll(np.roll(cover, dy, 0), dx, 1)
            out = np.maximum(out, shifted)
    return out


def indices(data, entry, rect):
    """The 4- or 8-bit palette indices of a reading's rectangle (x, y, w, h)."""
    x0, y0, w, h = rect
    stride = entry.get("stride") or entry["words"]
    rows = entry.get("row_offsets")
    out = np.zeros((h, w), np.int32)
    for y in range(h):
        at = entry["offset"] + (rows[y0 + y] if rows else (y0 + y) * stride * 2)
        for x in range(w):
            if entry["bpp"] == 8:
                out[y, x] = data[at + x0 + x]
                continue
            byte = data[at + (x0 + x) // 2]
            out[y, x] = byte >> 4 if (x0 + x) & 1 else byte & 15
    return out


def colours(data, clut, entries=16):
    return np.frombuffer(b"".join(extract_images.expand(c) for c in extract_images.read_palette(data, clut, entries)),
                         np.uint8).reshape(entries, 4).astype(np.float64)


def paint(picture, cover, colour):
    """`colour` over the picture (straight alpha) as much as `cover`."""
    a = cover[..., None] * colour[3] / 255
    under = picture[..., 3:4] / 255
    alpha = a + under * (1 - a)
    rgb = (colour[:3] * a + picture[..., :3] * under * (1 - a)) / np.maximum(alpha, 1e-6)
    picture[..., :3] = np.where(alpha > 0, rgb, 0)
    picture[..., 3:4] = alpha * 255


def draw_label(label, data, entry, reading, font_file):
    """A label set anew in the font, in place of the game's letters.
    `shadow`: a word (CHEST) in `fill`, with a shadow in the first of
    `shadow` (if any) a texel down and right, and under the fill an `edge`
    (if given) round it, as big as the game's letters (the texels of `rect`
    that are neither `background` nor `shadow`); every other texel of `rect`
    becomes the background round it, or stays clear where the background is
    the transparent entry 0. `outline`: characters, or words when `text` is a
    list, one per cell, in `fill` inside a texel of `outline`, each as tall
    as the game's."""
    palette = colours(data, entry["clut_offset"], 256 if entry["bpp"] == 8 else 16)
    if label["style"] == "strip":
        # A word the game cuts into pieces drawn side by side (MEAD + OW),
        # some through a letter: set once across the pieces laid in a row,
        # anti-aliased from `background` to `fill` as the game's letters are,
        # and cut back at the same places. Clear texels stay clear.
        rects = label["rects"]
        index = np.concatenate([indices(data, entry, r) for r in rects], 1)
        ys, xs = np.nonzero(~np.isin(index, [0] + label["background"]))
        x0, y0, x1, y1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
        ink = np.zeros((index.shape[0] * S, index.shape[1] * S))
        ink[y0 * S:y1 * S, x0 * S:x1 * S] = text_cover(label["text"], font_file, (x1 - x0) * S, (y1 - y0) * S)
        back, front = palette[label["background"][0]], palette[label["fill"]]
        picture = back * (1 - ink[..., None]) + front * ink[..., None]
        picture[..., 3] = blocks(index != 0) * 255
        at = 0
        for x, y, w, h in rects:
            reading.out[y * S:(y + h) * S, x * S:(x + w) * S] = picture[:, at * S:(at + w) * S].round().astype(np.uint8)
            at += w
        return
    if label["style"] == "shadow":
        x, y, w, h = label["rect"]
        index = indices(data, entry, label["rect"])
        shadows = label.get("shadow", [])
        clear = np.isin(index, label["background"])
        ys, xs = np.nonzero(~clear & ~np.isin(index, shadows))
        x0, y0, x1, y1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
        picture = blocks(fill_transparent(palette[index][..., :3], clear)).astype(np.float64)
        opaque = clear & (index != 0) if 0 in label["background"] else np.ones(index.shape, bool)
        picture = np.concatenate([picture, blocks(opaque)[..., None] * 255.0], -1)
        body = text_cover(label["text"], font_file, (x1 - x0) * S, (y1 - y0) * S)
        if shadows:
            shade = np.zeros((h * S, w * S))
            shade[(y0 + 1) * S:(y1 + 1) * S, (x0 + 1) * S:(x1 + 1) * S] = body[:(h - y0 - 1) * S, :(w - x0 - 1) * S]
            paint(picture, shade, palette[shadows[0]])
        ink = np.zeros((h * S, w * S))
        ink[y0 * S:y1 * S, x0 * S:x1 * S] = body
        if "edge" in label:
            paint(picture, grow(ink, S // 2), palette[label["edge"]])
        paint(picture, ink, palette[label["fill"]])
        reading.out[y * S:(y + h) * S, x * S:(x + w) * S] = picture.round().astype(np.uint8)
        return
    for character, (x, y, w, h) in zip(label["text"], label["cells"]):
        index = indices(data, entry, (x, y, w, h))
        ys, xs = np.nonzero(index)
        if not len(ys):
            continue
        x0, y0, x1, y1 = xs.min() + 1, ys.min() + 1, xs.max(), ys.max()
        body = text_cover(character, font_file, None, (y1 - y0) * S)
        if body.shape[1] > (x1 - x0) * S:
            body = text_cover(character, font_file, (x1 - x0) * S, (y1 - y0) * S)
        ink = np.zeros((h * S, w * S))
        left = round((x0 + x1) * S / 2 - body.shape[1] / 2)
        ink[y0 * S:y0 * S + body.shape[0], left:left + body.shape[1]] = body
        picture = np.zeros((h * S, w * S, 4))
        paint(picture, grow(ink, S), palette[label["outline"]])
        paint(picture, ink, palette[label["fill"]])
        if label.get("clip"):
            # Only over the game's own letter: the same texels are drawn plain
            # in one place and subtracted in another (the hand's numbers and
            # the life points), and a pack pixel over a clear texel is never
            # blended, so a letter reaching past the old one would show there
            # in the wrong colours.
            picture[..., 3] *= blocks(index != 0)
        reading.out[y * S:(y + h) * S, x * S:(x + w) * S] = picture.round().astype(np.uint8)


def label_font(label, default):
    """The label's own font (a fontconfig pattern, "serif:bold"), else the default."""
    pattern = label.get("font")
    if not pattern:
        return default
    if shutil.which("fc-match"):
        found = subprocess.run(["fc-match", "-f", "%{file}", pattern], capture_output=True, text=True)
        if found.returncode == 0 and os.path.isfile(found.stdout):
            return found.stdout
    if os.name == "nt" and "serif" in pattern and "sans" not in pattern:
        fonts = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
        for name in ("georgiab.ttf", "timesbd.ttf"):
            if os.path.isfile(os.path.join(fonts, name)):
                return os.path.join(fonts, name)
    return default


def main():
    global SCALER
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("recipe")
    parser.add_argument("--data", default="game/DATA")
    parser.add_argument("--out", help="the mod folder to write (default: tmp/pc/packs/<id>)")
    parser.add_argument("--upscaler")
    parser.add_argument("--model", default="realesrgan-x4plus")
    parser.add_argument("--model-share", type=float, default=0.6)
    parser.add_argument("--font", help="the bold font the labels are set in (default: the system's bold sans)")
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
            reading.entry = entry
            reading.name = entry["file"].replace("sheets/", "").replace("/", "-")
            readings.append(reading)
            entries.append(dict(entry, file=reading.name, alias=f"{recipe['name']}: {sheet['what']}"))
            if "part" in recipe:   # the HD mod's part (hd_assets_pack.py); a pack alone ignores it
                entries[-1]["setting"] = recipe["part"]
        print(f"{len(readings)} readings: model {upscaler} ({args.model}), xBR through ffmpeg")
        cuts = build(readings)
        if recipe.get("trim"):
            # Outside what the game draws with a reading, its texels four
            # times: nothing there is ever seen, and it keeps the files small.
            for reading in readings:
                keep = blocks(reading.used)[..., None]
                plain = blocks(np.dstack([reading.rgb, reading.mask * 255]).astype(np.uint8))
                reading.out = np.where(keep, reading.out, plain)
        labels = recipe.get("labels", [])
        if labels:
            font_file = args.font or default_font()
            data = {}
            for label in labels:
                wanted = {int(p, 16) for p in label["palettes"]}
                offsets = label["offset"] if isinstance(label["offset"], list) else [label["offset"]]
                offsets = {int(o, 16) for o in offsets}
                font = label_font(label, font_file)
                drawn = 0
                for reading in readings:
                    e = reading.entry
                    if e["archive"] == label["archive"] and e["offset"] in offsets \
                            and e["bpp"] == label.get("bpp", 4) and e.get("clut_offset") in wanted:
                        if e["archive"] not in data:
                            data[e["archive"]] = extractor.archive(e["archive"])
                        draw_label(label, data[e["archive"]], e, reading, font)
                        drawn += 1
                if drawn != len(wanted) * len(offsets):
                    sys.exit(f"label {label['what']}: {drawn} of {len(wanted) * len(offsets)} readings are in the recipe")
            print(f"{len(labels)} labels set in {font_file}")
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
