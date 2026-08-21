"""SVG -> LVGL v9 binary image converter (ROADMAP #34).

Icons live on the SD card, not compiled into flash (see CLAUDE.md, "Что НЕ
делать") -- LVGL loads them at runtime via LV_USE_FS_STDIO from a plain
POSIX path (widget.c: "A:<dir app>/<src>"). This script produces the exact
file format that loader expects: a 12-byte lv_image_header_t (see
managed_components/lvgl__lvgl/src/draw/lv_image_dsc.h) followed by raw
pixel data.

Only LV_COLOR_FORMAT_ARGB8888 is emitted (cf=0x10, 4 bytes/px, byte order
B,G,R,A per lv_color32_t) -- simplest to get right, and file size doesn't
matter on an SD card the way it does in the flash slot. If SD space ever
becomes a real constraint, RGB565A8 would roughly halve these files.

Source SVGs are expected to use plain "fill" colors or a single linear/radial
gradient per shape (id="a" style refs, as the google-weather-icons and most
icon sets do) -- gradients are flattened to their middle stop's solid color
before rendering: at the ~40px sizes this project actually displays icons
at, a two-stop gradient is barely visible anyway, and MuPDF's built-in SVG
support does not resolve <linearGradient>/<radialGradient> refs at all (it
silently falls back to black rather than erroring, which is what actually
surfaced this — see ROADMAP #34).

Usage:
    python tools/icon_convert.py <input.svg> <output.bin> [size_px]

Requires: pymupdf, pillow (both already available in this environment).
"""
import re
import sys

import fitz
from PIL import Image

LV_IMAGE_HEADER_MAGIC = 0x19
LV_COLOR_FORMAT_ARGB8888 = 0x10


def flatten_gradients(svg_text):
    """Replace every fill="url(#id)" with a flat color from that gradient's
    middle stop. See module docstring for why this is necessary at all."""
    grad_colors = {}
    for m in re.finditer(
        r'<(?:linear|radial)Gradient[^>]*\bid="([^"]+)"[^>]*>(.*?)</(?:linear|radial)Gradient>',
        svg_text, re.S,
    ):
        gid, body = m.group(1), m.group(2)
        stops = re.findall(r'stop-color="([^"]+)"', body)
        if stops:
            grad_colors[gid] = stops[len(stops) // 2]

    def repl(m):
        return f'fill="{grad_colors[m.group(1)]}"' if m.group(1) in grad_colors else m.group(0)

    return re.sub(r'fill="url\(#([^)]+)\)"', repl, svg_text)


def render_svg(svg_path, size_px):
    """Returns a PIL RGBA Image, size_px x size_px, transparent background."""
    svg_text = open(svg_path, encoding="utf-8").read()
    flattened = flatten_gradients(svg_text).encode("utf-8")

    doc = fitz.open(stream=flattened, filetype="svg")
    page = doc[0]
    zoom = size_px / page.rect.width
    pix = page.get_pixmap(matrix=fitz.Matrix(zoom, zoom), alpha=True)

    img = Image.frombytes("RGBA", (pix.width, pix.height), pix.samples)
    if (pix.width, pix.height) != (size_px, size_px):
        img = img.resize((size_px, size_px), Image.LANCZOS)
    return img


def write_lv_bin(img, out_path):
    """img: PIL RGBA Image. Writes an LVGL v9 ARGB8888 .bin file."""
    w, h = img.size
    r, g, b, a = img.split()
    bgra = Image.merge("RGBA", (b, g, r, a)).tobytes()  # lv_color32_t byte order

    stride = w * 4
    header = (
        (LV_IMAGE_HEADER_MAGIC | (LV_COLOR_FORMAT_ARGB8888 << 8) | (0 << 16)).to_bytes(4, "little")
        + (w | (h << 16)).to_bytes(4, "little")
        + (stride | (0 << 16)).to_bytes(4, "little")
    )
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(bgra)


def convert(svg_path, out_path, size_px=40):
    img = render_svg(svg_path, size_px)
    write_lv_bin(img, out_path)
    return img


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    svg_in = sys.argv[1]
    bin_out = sys.argv[2]
    px = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    convert(svg_in, bin_out, px)
    print(f"wrote {bin_out} ({px}x{px})")
