#!/usr/bin/env python3
"""
Generates two solid mid-tone PNGs used by integration_blend.

Regeneration:
    cd test_media/scripts && python generate_blend_fixtures.py

Output:
    test_media/blend_bg/frame_000.png  - solid (0.50, 0.25, 0.75)
    test_media/blend_fg/frame_000.png  - solid (0.40, 0.60, 0.20)

Mid-tone (non-saturated) channels are required: pure 0/1 channel values are
fixed points for Normal/Add/Multiply/Screen, so blend modes would produce
identical pixels and the test could not distinguish them.

With these two colors the four supported modes all produce distinct outputs:
    Normal   = fg                                -> (0.40, 0.60, 0.20)
    Add      = bg + fg (premul, alpha=1, clamp)  -> (0.90, 0.85, 0.95)
    Multiply = bg * fg                           -> (0.20, 0.15, 0.15)
    Screen   = 1 - (1-bg)*(1-fg)                 -> (0.70, 0.70, 0.80)
"""
from pathlib import Path
from PIL import Image

SIZE = 64
ROOT = Path(__file__).resolve().parent.parent

FIXTURES = {
    "blend_bg": (0.50, 0.25, 0.75),
    "blend_fg": (0.40, 0.60, 0.20),
}


def main() -> None:
    for name, (r, g, b) in FIXTURES.items():
        out_dir = ROOT / name
        out_dir.mkdir(parents=True, exist_ok=True)
        color = (int(round(r * 255)), int(round(g * 255)), int(round(b * 255)), 255)
        img = Image.new("RGBA", (SIZE, SIZE), color)
        out = out_dir / "frame_000.png"
        img.save(out, format="PNG", optimize=True)
        print(f"  {out.relative_to(ROOT.parent)}  {color}")


if __name__ == "__main__":
    main()
