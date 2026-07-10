#!/usr/bin/env python3
"""Generate raster app icons from the DendroLog ring design.

The vector original lives in resources/icons/dendrolog.svg; this script
redraws the same design with Pillow (no SVG rasteriser needed) and writes:

  resources/windows/dendrolog.ico   -- multi-size icon for the exe / installer
  packaging/linux/dendrolog.png     -- 256px icon for the .desktop entry

Small sizes use simplified ring sets: four hairline rings turn to mush at
16 px, so 16/20/24 get two thick rings and 32/40 get three.

Run from the repository root:  python scripts/generate_icons.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent

WOOD = (234, 217, 192, 255)     # ring colour (aged wood)
ACCENT = (77, 201, 162, 255)    # the highlighted "found entry" ring
BARK = (78, 52, 46, 255)        # background (bark brown)

SS = 8  # supersampling factor


def design(size):
    """Return (rings, pith) tuned for the target size.

    rings: list of (cx, cy, r, stroke_width, colour) in 256-unit design space.
    pith:  (cx, cy, r) of the filled centre dot.
    """
    if size < 24:
        return ([(123, 133, 76, 22, WOOD),
                 (128, 128, 36, 24, ACCENT)],
                (128, 126, 12))
    if size < 48:
        return ([(122, 134, 84, 15, WOOD),
                 (126, 130, 51, 16, ACCENT),
                 (129, 127, 23, 13, WOOD)],
                (129, 126, 8))
    return ([(122, 134, 86, 11, WOOD),
             (125, 131, 62, 12, WOOD),
             (127, 129, 40, 13, ACCENT),
             (129, 127, 20, 11, WOOD)],
            (130, 126, 7))


def render(size):
    s = size * SS
    u = s / 256.0
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=56 * u, fill=BARK)

    rings, pith = design(size)
    for cx, cy, r, w, colour in rings:
        # Keep every ring at least ~1.4 device pixels wide after downscaling.
        w_px = max(w * u, 1.4 * SS)
        box = [cx * u - r * u, cy * u - r * u, cx * u + r * u, cy * u + r * u]
        d.ellipse(box, outline=colour, width=round(w_px))

    cx, cy, r = pith
    d.ellipse([cx * u - r * u, cy * u - r * u, cx * u + r * u, cy * u + r * u],
              fill=WOOD)

    return img.resize((size, size), Image.LANCZOS)


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = {size: render(size) for size in sizes}

    ico_path = ROOT / "resources" / "windows" / "dendrolog.ico"
    ico_path.parent.mkdir(parents=True, exist_ok=True)
    base = images[256]
    base.save(ico_path, format="ICO",
              append_images=[images[s] for s in sizes if s != 256])
    print(f"wrote {ico_path}")

    png_path = ROOT / "packaging" / "linux" / "dendrolog.png"
    png_path.parent.mkdir(parents=True, exist_ok=True)
    images[256].save(png_path, format="PNG")
    print(f"wrote {png_path}")


if __name__ == "__main__":
    main()
