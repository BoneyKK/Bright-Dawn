#!/usr/bin/env python3
"""Makes the program's icon files from src/app/icon/bright-dawn.svg:

- src/app/icon/bright-dawn.ico: the icon of the Windows program, drawn at
  each size Windows asks for (bright-dawn.rc puts it in the .exe).
- src/app/window_icon.h: the window's icon, as PNG files of a few sizes,
  for SDL_SetWindowIcon (app/main.cpp).

The files are kept in the repository, so a build needs none of this. Run it
after a change to the SVG; `--check` says whether they are as the SVG makes
them. Needs cairosvg (and its Cairo library) and Pillow.
"""
import io
import pathlib
import sys

import cairosvg
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
SVG = ROOT / "src" / "app" / "icon" / "bright-dawn.svg"
ICO = ROOT / "src" / "app" / "icon" / "bright-dawn.ico"
HEADER = ROOT / "src" / "app" / "window_icon.h"
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
WINDOW_SIZES = [256, 64, 32, 16]  # The first is the main image; SDL picks among them.


def drawn(size: int) -> Image.Image:
    png = cairosvg.svg2png(bytestring=SVG.read_bytes(), output_width=size, output_height=size)
    return Image.open(io.BytesIO(png)).convert("RGBA")


def png_bytes(image: Image.Image) -> bytes:
    out = io.BytesIO()
    image.save(out, format="PNG", optimize=True)
    return out.getvalue()


def ico() -> bytes:
    images = [drawn(size) for size in ICO_SIZES]
    out = io.BytesIO()
    images[-1].save(out, format="ICO", sizes=[(s, s) for s in ICO_SIZES], append_images=images[:-1])
    return out.getvalue()


def header() -> str:
    lines = [
        "// Made by tools/icon/make_icon.py from src/app/icon/bright-dawn.svg: do not edit.",
        "// The window's icon: PNG files, largest first.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace bd::app::window_icon {",
        "",
    ]
    names = []
    for size in WINDOW_SIZES:
        data = png_bytes(drawn(size))
        name = f"kPng{size}"
        names.append(name)
        lines.append(f"inline constexpr std::uint8_t {name}[] = {{")
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
        lines.append("};")
        lines.append("")
    lines.append("struct Png {")
    lines.append("  const std::uint8_t* data;")
    lines.append("  std::size_t size;")
    lines.append("};")
    lines.append("inline constexpr Png kPngs[] = {" + ", ".join(f"{{{n}, sizeof {n}}}" for n in names) + "};")
    lines.append("")
    lines.append("}  // namespace bd::app::window_icon")
    return "\n".join(lines) + "\n"


def main() -> int:
    made = {ICO: ico(), HEADER: header().encode()}
    if "--check" in sys.argv[1:]:
        stale = [str(path) for path, data in made.items() if not path.exists() or path.read_bytes() != data]
        for path in stale:
            print(f"{path} is not as the SVG makes it: run tools/icon/make_icon.py")
        return 1 if stale else 0
    for path, data in made.items():
        path.write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
