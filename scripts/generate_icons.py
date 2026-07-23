#!/usr/bin/env python3
"""Generate raster icons from the DendroLog ring design.

The vector originals live in resources/icons/; this script redraws the same
designs with Pillow (no SVG rasteriser needed) and writes:

  resources/windows/dendrolog.ico       -- app icon for the exe / installer
  resources/windows/dendrolog-log.ico   -- icon of associated .log files
  packaging/linux/dendrolog.png         -- 256px icon for the .desktop entry

Small sizes use simplified ring sets: four hairline rings turn to mush at
16 px, so 16/20/24 get two thick rings and 32/40 get three. The document
icon drops its text lines below 24 px for the same reason.

Run from the repository root:  python scripts/generate_icons.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent

WOOD = (234, 217, 192, 255)     # ring colour (aged wood)
ACCENT = (77, 201, 162, 255)    # the highlighted "found entry" ring
BARK = (78, 52, 46, 255)        # background (bark brown)

PAPER = (255, 255, 255, 255)    # document sheet
PAPER_EDGE = (163, 171, 186, 255)
FOLD = (214, 221, 231, 255)     # the folded-over corner
LINE = (176, 184, 197, 255)     # "text" on the sheet

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


def doc_design(size):
    """Return (page, fold, lines, emblem_fraction, edge_width).

    page:  (left, top, right, bottom) of the sheet in 256-unit design space.
    lines: list of (x0, x1, y, height) text strokes, same space.
    emblem_fraction: emblem side as a share of the icon side.

    The badge keeps a usable size at every scale, so the sheet has to grow to
    stay visible under it: at 16 px it spans nearly the whole canvas, while at
    48 px and up it can sit inset with proper document proportions.
    """
    if size < 24:
        # 16/20 px: hairline text turns to grey mush — sheet plus badge only.
        return ((16, 6, 214, 250), 46, [], 0.58, 12)
    if size < 48:
        return ((28, 10, 212, 246), 50,
                [(56, 168, 112, 14),
                 (56, 168, 148, 14)],
                0.54, 9)
    return ((42, 14, 208, 238), 56,
            [(66, 182, 96, 11),
             (66, 182, 124, 11),
             (66, 152, 152, 11),
             (66, 116, 180, 11),
             (66, 120, 208, 11)],
            0.46, 6)


def render_document(size):
    """A sheet of paper with the ring emblem badged onto its corner."""
    s = size * SS
    u = s / 256.0
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    page, fold_size, lines, emblem_frac, edge_w = doc_design(size)
    left, top, right, bottom = (v * u for v in page)
    fold = fold_size * u
    # Hairlines below ~1.4 device pixels wash out to nothing when downscaled.
    edge_px = round(max(edge_w * u, 1.4 * SS))

    sheet = [(left, top), (right - fold, top), (right, top + fold),
             (right, bottom), (left, bottom)]
    d.polygon(sheet, fill=PAPER)
    d.line(sheet + [sheet[0]], fill=PAPER_EDGE, width=edge_px, joint="curve")

    flap = [(right - fold, top), (right, top + fold), (right - fold, top + fold)]
    d.polygon(flap, fill=FOLD)
    d.line(flap + [flap[0]], fill=PAPER_EDGE, width=edge_px, joint="curve")

    for x0, x1, y, h in lines:
        r = max(h * u / 2.0, 0.7 * SS)
        d.rounded_rectangle([x0 * u, y * u - r, x1 * u, y * u + r],
                            radius=r, fill=LINE)

    emblem_px = max(round(size * emblem_frac), 8)
    ex, ey = size - emblem_px, size - emblem_px

    # White gutter so the badge reads as a separate object where it overlaps
    # the sheet; it runs off the bottom-right corner, so only two sides show.
    # Its radius follows the badge's own corner (56 units of an emblem-sized
    # canvas), not the icon's — the two differ by the emblem fraction.
    halo = max(size * 0.016, 1.0) * SS
    d.rounded_rectangle([ex * SS - halo, ey * SS - halo, s, s],
                        radius=56.0 / 256.0 * emblem_px * SS + halo, fill=PAPER)

    img = img.resize((size, size), Image.LANCZOS)

    overlay = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    overlay.paste(render(emblem_px), (ex, ey))
    return Image.alpha_composite(img, overlay)


def save_ico(path, images, sizes):
    path.parent.mkdir(parents=True, exist_ok=True)
    base = images[sizes[-1]]
    # `sizes` must be passed explicitly: Pillow otherwise writes its own
    # default set and silently drops 20/40 px (the 125 % DPI shell sizes).
    base.save(path, format="ICO", sizes=[(s, s) for s in sizes],
              append_images=[images[s] for s in sizes[:-1]])
    print(f"wrote {path}")


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]

    app = {size: render(size) for size in sizes}
    save_ico(ROOT / "resources" / "windows" / "dendrolog.ico", app, sizes)

    doc = {size: render_document(size) for size in sizes}
    save_ico(ROOT / "resources" / "windows" / "dendrolog-log.ico", doc, sizes)

    png_path = ROOT / "packaging" / "linux" / "dendrolog.png"
    png_path.parent.mkdir(parents=True, exist_ok=True)
    app[256].save(png_path, format="PNG")
    print(f"wrote {png_path}")


if __name__ == "__main__":
    main()
