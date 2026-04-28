#!/usr/bin/env python3
"""
Generates the ACES smoke test pattern PNG used by integration_aces_smoke.

Regeneration:
    cd test_media/scripts && python generate_aces_test_pattern.py

Output:
    test_media/aces_test_pattern/frame_000.png  -  64x64 RGBA

The fixture lives in its own subdirectory so PNGSequenceDecoder's
parent_path() scan only sees this one frame -- adding a new top-level
test_media PNG later wouldn't accidentally extend the "sequence".

Pattern layout (16-row horizontal bands, sRGB-encoded byte values):
    rows  0-15   horizontal black-to-white gradient (0 -> 255 across columns)
    rows 16-31   solid sRGB mid-gray (188, 188, 188) -- linearizes to ~0.5
    rows 32-47   pure primaries (red 0-21, green 22-42, blue 43-63)
    rows 48-63   pure secondaries (magenta, cyan, yellow) over thirds

Why these specific values:
    * The horizontal gradient exercises the OETF over its full domain.
    * Mid-gray 188 is the standard sRGB "perceptual middle" anchor; after
      linearization (sRGB EOTF) it sits near 0.5 in scene-linear, which
      lands well clear of the ACES ODT's shoulder.
    * Pure primaries gate channel-order regressions in the input -> ACEScg
      transform (a swizzle would surface here via the integration golden).
    * Pure secondaries gate per-channel mixing in the working space (ACEScg
      primaries are NOT identical to Rec.709, so the per-channel triplets
      do change measurably through the input transform).

This is a regression sentinel for the OCIO ODT pipeline -- the value of the
test is "the same input always produces the same hash". The specific
numeric output is whatever the active config emits and gets baked into
tests/goldens/aces_smoke/frame_0.hash.
"""
from pathlib import Path
from PIL import Image

SIZE = 64
BAND = SIZE // 4  # 16 rows per band
ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "aces_test_pattern"
OUT = OUT_DIR / "frame_000.png"


def main() -> None:
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 255))
    px = img.load()

    # Band 0 (rows 0-15): horizontal gradient 0 -> 255.
    for y in range(0, BAND):
        for x in range(SIZE):
            v = int(round(x * 255.0 / (SIZE - 1)))
            px[x, y] = (v, v, v, 255)

    # Band 1 (rows 16-31): solid sRGB mid-gray.
    for y in range(BAND, 2 * BAND):
        for x in range(SIZE):
            px[x, y] = (188, 188, 188, 255)

    # Band 2 (rows 32-47): primaries split into thirds.
    third = SIZE // 3
    for y in range(2 * BAND, 3 * BAND):
        for x in range(SIZE):
            if x < third:
                px[x, y] = (255, 0, 0, 255)
            elif x < 2 * third:
                px[x, y] = (0, 255, 0, 255)
            else:
                px[x, y] = (0, 0, 255, 255)

    # Band 3 (rows 48-63): secondaries split into thirds.
    for y in range(3 * BAND, SIZE):
        for x in range(SIZE):
            if x < third:
                px[x, y] = (255, 0, 255, 255)   # magenta
            elif x < 2 * third:
                px[x, y] = (0, 255, 255, 255)   # cyan
            else:
                px[x, y] = (255, 255, 0, 255)   # yellow

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    img.save(OUT, format="PNG", optimize=True)
    print(f"  {OUT.relative_to(ROOT.parent)}  64x64 RGBA")


if __name__ == "__main__":
    main()
