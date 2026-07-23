#!/usr/bin/env python3
"""
Build the application icon from the game's own artwork.

Cosmo is lifted out of the 1992 title screen rather than redrawn, so the icon
is the character as Apogee's artists drew him. Mode 0Dh gives only sixteen
colours, and the ones the figure is drawn in differ from the ones behind him,
which is what makes separating the two reliable: a flood fill over his palette
picks up the whole character and nothing else.

    python3 tools/make-icon.py

Writes assets/icon.png, assets/icon.icns and assets/icon.ico. They are
committed, so neither the build nor CI needs image tooling. Run this again only
if the icon should change.
"""

import os
import subprocess
import sys
import tempfile
from collections import deque

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("this needs Pillow: pip install pillow")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(REPO, "docs", "screenshots", "title1.png")
OUT_DIR = os.path.join(REPO, "assets")

# Cosmo stands here on the title screen. The box is tight around him on
# purpose: the foliage to his left is drawn in the same darker green as his own
# shading, and excluding it by position is simpler than by colour.
FIGURE_BOX = (300, 244, 381, 390)

# The colours he is drawn in.
FIGURE_COLOURS = {
    (85, 255, 85), (0, 170, 0),        # body, and its shading
    (170, 0, 0), (255, 85, 85),        # crest, scarf, spots
    (255, 255, 255), (170, 170, 170), (85, 85, 85),  # eyes
}
OUTLINE = (0, 0, 0)

# Sky from the same screen, for the background the figure sits on.
SKY_TOP = (85, 255, 255)
SKY_BOTTOM = (0, 170, 170)

CANVAS = 1024
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
ICNS_SIZES = [16, 32, 64, 128, 256, 512, 1024]


def extract_figure():
    """Return Cosmo alone, on transparency."""
    image = Image.open(SOURCE).convert("RGB")
    px = image.load()

    x0, y0, x1, y1 = FIGURE_BOX
    width, height = x1 - x0, y1 - y0

    def colour(x, y):
        return px[x0 + x, y0 + y]

    # Flood fill his palette, keeping only substantial regions so that stray
    # scenery pixels of the same colour are left behind.
    seen = [[False] * width for _ in range(height)]
    keep = [[False] * width for _ in range(height)]

    for start_y in range(height):
        for start_x in range(width):
            if seen[start_y][start_x] or colour(start_x, start_y) not in FIGURE_COLOURS:
                continue

            region = []
            queue = deque([(start_x, start_y)])
            seen[start_y][start_x] = True

            while queue:
                x, y = queue.popleft()
                region.append((x, y))
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if not (0 <= nx < width and 0 <= ny < height):
                        continue
                    if seen[ny][nx] or colour(nx, ny) not in FIGURE_COLOURS:
                        continue
                    seen[ny][nx] = True
                    queue.append((nx, ny))

            if len(region) >= 200:
                for x, y in region:
                    keep[y][x] = True

    # One step outwards, to take in the black line he is drawn with.
    for y in range(height):
        for x in range(width):
            if not keep[y][x]:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1),
                           (1, 1), (-1, -1), (1, -1), (-1, 1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < width and 0 <= ny < height and colour(nx, ny) == OUTLINE:
                    keep[ny][nx] = True

    columns = [x for y in range(height) for x in range(width) if keep[y][x]]
    rows = [y for y in range(height) for x in range(width) if keep[y][x]]
    left, right = min(columns), max(columns)
    top, bottom = min(rows), max(rows)

    figure = Image.new("RGBA", (right - left + 1, bottom - top + 1), (0, 0, 0, 0))
    out = figure.load()
    for y in range(top, bottom + 1):
        for x in range(left, right + 1):
            if keep[y][x]:
                out[x - left, y - top] = colour(x, y) + (255,)

    return figure


def compose(figure):
    """Cosmo on a rounded square of sky, sized for an application icon."""
    icon = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))

    sky = Image.new("RGBA", (CANVAS, CANVAS))
    paint = sky.load()
    for y in range(CANVAS):
        t = y / (CANVAS - 1)
        paint_row = tuple(
            round(SKY_TOP[i] + (SKY_BOTTOM[i] - SKY_TOP[i]) * t) for i in range(3)
        )
        for x in range(CANVAS):
            paint[x, y] = paint_row + (255,)

    # The rounded square macOS expects; harmless everywhere else.
    mask = Image.new("L", (CANVAS, CANVAS), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (CANVAS * 0.06, CANVAS * 0.06, CANVAS * 0.94, CANVAS * 0.94),
        radius=CANVAS * 0.22, fill=255)
    icon.paste(sky, (0, 0), mask)

    # Scaled by a whole number, so the pixels stay square.
    usable = CANVAS * 0.62
    scale = max(1, int(min(usable / figure.width, usable / figure.height)))
    scaled = figure.resize(
        (figure.width * scale, figure.height * scale), Image.NEAREST)

    icon.paste(scaled,
               ((CANVAS - scaled.width) // 2, (CANVAS - scaled.height) // 2),
               scaled)
    return icon


def write_icns(icon, path):
    """iconutil is the only thing that writes .icns, and it is macOS-only."""
    if sys.platform != "darwin":
        print("  .icns skipped: needs macOS")
        return False

    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "icon.iconset")
        os.makedirs(iconset)

        for size in ICNS_SIZES:
            icon.resize((size, size), Image.LANCZOS).save(
                os.path.join(iconset, f"icon_{size}x{size}.png"))
            if size <= 512:
                icon.resize((size * 2, size * 2), Image.LANCZOS).save(
                    os.path.join(iconset, f"icon_{size}x{size}@2x.png"))

        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", path], check=True)
    return True


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    figure = extract_figure()
    print(f"figure lifted from the title screen: {figure.width}x{figure.height}")

    icon = compose(figure)

    png = os.path.join(OUT_DIR, "icon.png")
    icon.save(png)
    print(f"wrote {png}")

    ico = os.path.join(OUT_DIR, "icon.ico")
    icon.save(ico, sizes=[(s, s) for s in ICO_SIZES])
    print(f"wrote {ico}")

    icns = os.path.join(OUT_DIR, "icon.icns")
    if write_icns(icon, icns):
        print(f"wrote {icns}")


if __name__ == "__main__":
    main()
