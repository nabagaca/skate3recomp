#!/usr/bin/env python3
"""Decode debug-dumped Xbox 360 tiled RGBA8 resolve textures to PNGs."""

from __future__ import annotations

import argparse
import html
import re
from pathlib import Path

try:
    from PIL import Image
except ImportError as exc:
    raise SystemExit("Pillow is required to write PNGs: python3 -m pip install --user pillow") from exc


DUMP_NAME_RE = re.compile(r"addr_([0-9a-fA-F]{8})_len_([0-9a-fA-F]+)_")

# These are the render-target resolve ranges observed around the Import Skater
# preset-preview path. Pitch is in pixels, including Xbox 360 tile padding.
KNOWN_SURFACES_BY_RANGE = {
    (0x04614000, 0x02D000): (288, 160, 288),
    (0x0460E000, 0x006000): (72, 40, 96),
    (0x0460A000, 0x001000): (32, 16, 32),
    (0x0460B000, 0x001000): (32, 16, 32),
    (0x04911000, 0x2D0000): (1152, 640, 1152),
    (0x04641000, 0x2D0000): (1152, 640, 1152),
    (0x04BE1000, 0x2D0000): (1152, 640, 1152),
    (0x04EB1000, 0x2D0000): (1152, 640, 1152),
    (0x04BE1000, 0x168000): (1152, 320, 1152),
    (0x04D49000, 0x168000): (1152, 320, 1152),
    (0x04EB1000, 0x168000): (1152, 320, 1152),
    (0x05019000, 0x168000): (1152, 320, 1152),
    (0x05258000, 0x398000): (1280, 720, 1280),
    (0x03554000, 0x300000): (1024, 768, 1024),
    (0x03954000, 0x300000): (1024, 768, 1024),
    (0x03C54000, 0x100000): (512, 512, 512),
}

KNOWN_SURFACES_BY_LENGTH = {
    0x02D000: (288, 160, 288),
    0x006000: (72, 40, 96),
    0x001000: (32, 16, 32),
    0x2D0000: (1152, 640, 1152),
    0x168000: (1152, 320, 1152),
    0x398000: (1280, 720, 1280),
    0x384000: (1280, 720, 1280),
    0x300000: (1024, 768, 1024),
    0x100000: (512, 512, 512),
}


def tiled_offset_2d_row(y: int, pitch: int, log2_bpp: int) -> int:
    macro = ((y // 32) * (pitch // 32)) << (log2_bpp + 7)
    micro = ((y & 6) << 2) << log2_bpp
    return (
        macro
        + ((micro & ~0xF) << 1)
        + (micro & 0xF)
        + ((y & 8) << (3 + log2_bpp))
        + ((y & 1) << 4)
    )


def tiled_offset_2d_column(x: int, y: int, log2_bpp: int, base_offset: int) -> int:
    macro = (x // 32) << (log2_bpp + 7)
    micro = (x & 7) << log2_bpp
    offset = base_offset + (macro + ((micro & ~0xF) << 1) + (micro & 0xF))
    return (
        ((offset & ~0x1FF) << 3)
        + ((offset & 0x1C0) << 2)
        + (offset & 0x3F)
        + ((y & 16) << 7)
        + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)
    )


def parse_dump_name(path: Path) -> tuple[int, int] | None:
    match = DUMP_NAME_RE.search(path.name)
    if not match:
        return None
    return int(match.group(1), 16), int(match.group(2), 16)


def infer_surface(address: int, length: int) -> tuple[int, int, int] | None:
    surface = KNOWN_SURFACES_BY_RANGE.get((address, length))
    if surface:
        return surface
    surface = KNOWN_SURFACES_BY_LENGTH.get(length)
    if surface:
        return surface

    pixels = length // 4
    for pitch in (1280, 1152, 1024, 768, 720, 640, 512, 384, 320, 288, 256, 128, 96, 64, 32):
        if pixels % pitch == 0:
            height = pixels // pitch
            if height > 0:
                return pitch, height, pitch
    return None


def untile_rgba8(raw: bytes, width: int, height: int, pitch: int) -> bytearray:
    output = bytearray(width * height * 4)
    for y in range(height):
        row_offset = tiled_offset_2d_row(y, pitch, 2)
        for x in range(width):
            input_offset = tiled_offset_2d_column(x, y, 2, row_offset)
            input_offset = (input_offset >> 2) * 4
            if input_offset + 4 > len(raw):
                continue
            output_offset = (y * width + x) * 4
            output[output_offset : output_offset + 4] = raw[input_offset : input_offset + 4]
    return output


def save_variants(untiled: bytearray, width: int, height: int, output_stem: Path) -> list[Path]:
    rgba = Image.frombytes("RGBA", (width, height), bytes(untiled))

    bgra_bytes = bytearray(len(untiled))
    for i in range(0, len(untiled), 4):
        bgra_bytes[i + 0] = untiled[i + 2]
        bgra_bytes[i + 1] = untiled[i + 1]
        bgra_bytes[i + 2] = untiled[i + 0]
        bgra_bytes[i + 3] = untiled[i + 3]
    bgra = Image.frombytes("RGBA", (width, height), bytes(bgra_bytes))

    alpha_rgb = bytearray(untiled)
    for i in range(3, len(alpha_rgb), 4):
        alpha_rgb[i] = 255
    rgba_opaque = Image.frombytes("RGBA", (width, height), bytes(alpha_rgb))

    paths = [
        output_stem.with_name(output_stem.name + "_rgba.png"),
        output_stem.with_name(output_stem.name + "_bgra.png"),
        output_stem.with_name(output_stem.name + "_rgba_opaque.png"),
    ]
    rgba.save(paths[0])
    bgra.save(paths[1])
    rgba_opaque.save(paths[2])
    return paths


def write_index(rows: list[tuple[Path, int, int, int, int, list[Path]]], output_dir: Path) -> None:
    index_path = output_dir / "index.html"
    parts = [
        "<!doctype html><meta charset='utf-8'><title>Texture dumps</title>",
        "<style>body{font-family:sans-serif;margin:24px;background:#111;color:#eee}"
        "section{margin:0 0 32px}img{max-width:360px;image-rendering:auto;background:#444}"
        ".row{display:flex;gap:16px;flex-wrap:wrap}.item{max-width:380px}"
        "code{color:#b7f7ff}</style>",
        "<h1>Texture dumps</h1>",
    ]
    for raw_path, address, length, width, height, images in rows:
        parts.append(
            f"<section><h2><code>{html.escape(raw_path.name)}</code></h2>"
            f"<p>address <code>0x{address:08X}</code>, length <code>0x{length:X}</code>, "
            f"{width}x{height}</p><div class='row'>"
        )
        for image_path in images:
            label = image_path.name.removeprefix(raw_path.stem + "_").removesuffix(".png")
            parts.append(
                f"<div class='item'><p>{html.escape(label)}</p>"
                f"<img src='{html.escape(image_path.name)}'></div>"
            )
        parts.append("</div></section>")
    index_path.write_text("\n".join(parts), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "input_dir",
        nargs="?",
        default="/home/alex/Desktop/skate_fresh/logs/texture_dumps/raw",
        help="Directory containing resolve_*.bin dumps",
    )
    parser.add_argument(
        "--output-dir",
        default="/home/alex/Desktop/skate_fresh/logs/texture_dumps/rendered",
        help="Directory to write decoded PNGs and index.html",
    )
    args = parser.parse_args()

    input_dir = Path(args.input_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    rows: list[tuple[Path, int, int, int, int, list[Path]]] = []
    for raw_path in sorted(input_dir.glob("resolve_*.bin")):
        parsed = parse_dump_name(raw_path)
        if not parsed:
            continue
        address, length = parsed
        surface = infer_surface(address, length)
        if not surface:
            print(f"Skipping {raw_path.name}: unknown dimensions for length 0x{length:X}")
            continue
        width, height, pitch = surface
        raw = raw_path.read_bytes()
        untiled = untile_rgba8(raw, width, height, pitch)
        output_stem = output_dir / raw_path.stem
        images = save_variants(untiled, width, height, output_stem)
        rows.append((raw_path, address, length, width, height, images))
        print(f"Decoded {raw_path.name}: {width}x{height}, pitch {pitch}")

    write_index(rows, output_dir)
    print(f"Wrote {output_dir / 'index.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
