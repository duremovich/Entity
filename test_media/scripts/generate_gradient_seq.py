#!/usr/bin/env python3
"""
Generates a deterministic PNG sequence for integration tests.

Regeneration:
    cd test_media/scripts && python generate_gradient_seq.py

Output: test_media/gradient_seq/frame_000.png .. frame_015.png
Each frame is a solid HSV(hue=i*22.5, sat=1, val=1) color, 64x64 RGBA.
Solid colors keep hashes trivially deterministic and make per-frame
identity easy to verify visually.
"""
from pathlib import Path
from colorsys import hsv_to_rgb
from PIL import Image

FRAME_COUNT = 16
SIZE = 64
OUT_DIR = Path(__file__).resolve().parent.parent / "gradient_seq"


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for i in range(FRAME_COUNT):
        h = i / FRAME_COUNT
        r, g, b = hsv_to_rgb(h, 1.0, 1.0)
        color = (int(r * 255), int(g * 255), int(b * 255), 255)
        img = Image.new("RGBA", (SIZE, SIZE), color)
        out = OUT_DIR / f"frame_{i:03d}.png"
        img.save(out, format="PNG", optimize=True)
        print(f"  {out.name}  {color}")
    print(f"Wrote {FRAME_COUNT} frames to {OUT_DIR}")


if __name__ == "__main__":
    main()
