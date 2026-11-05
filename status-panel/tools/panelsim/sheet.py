#!/usr/bin/env python3
"""Compose panelSim PPM frames into an LED-look contact sheet (PNG)."""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

SCALE = 9
PAD = 16
LABEL = 18
UNLIT = (38, 36, 32)  # unlit packages read pale in a room; keep them visible


def led(frame):
    w, h = frame.size
    out = Image.new("RGB", (w * SCALE, h * SCALE), (10, 10, 11))
    draw = ImageDraw.Draw(out)
    px = frame.load()
    for y in range(h):
        for x in range(w):
            c = px[x, y]
            fill = c if max(c) > 8 else UNLIT
            draw.ellipse([x * SCALE + 1, y * SCALE + 1,
                          x * SCALE + SCALE - 2, y * SCALE + SCALE - 2], fill=fill)
    return out


def main(src, dest, cols=2):
    files = sorted(Path(src).glob("*.ppm"))
    cols = int(cols)
    w = 53 * SCALE + PAD
    h = 11 * SCALE + PAD + LABEL
    rows = (len(files) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * w + PAD, rows * h + PAD), (24, 25, 28))
    draw = ImageDraw.Draw(sheet)
    for i, f in enumerate(files):
        x = PAD + (i % cols) * w
        y = PAD + (i // cols) * h
        draw.text((x, y), f.stem, fill=(220, 225, 232))
        sheet.paste(led(Image.open(f).convert("RGB")), (x, y + LABEL))
    sheet.save(dest)


if __name__ == "__main__":
    main(*sys.argv[1:])
