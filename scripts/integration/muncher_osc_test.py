#!/usr/bin/env python3
"""
Generative-layer smoke test for the Muncher layer.

Launches the headless editor with `muncher_osc_wait.json` and verifies that
the Muncher layer is created and that two compose-target screenshots land on
disk (before / after the wait window).

Note: the global /entity/muncher/* OSC input routes were removed in Phase 4
of ADR-0028 (all muncher input control now goes through the per-layer remote
namespace /entity/layer/{id}/... after patching). Muncher input control via
OSC will be revisited when the generative layer system is reworked. The
per-layer remote control path is exercised by remote_layer_osc_test.py.

Usage:
    python scripts/integration/muncher_osc_test.py

Returns nonzero on any unmet assertion.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO_ROOT     = Path(__file__).resolve().parents[2]
EDITOR_EXE    = REPO_ROOT / "build" / "bin" / "Release" / "EntityMediaEditor.exe"
SCRIPT_PATH   = REPO_ROOT / "scripts" / "integration" / "muncher_osc_wait.json"
DEBUG_DIR     = REPO_ROOT / "debug"


def main() -> int:
    if not EDITOR_EXE.exists():
        print(f"FAIL: editor binary missing: {EDITOR_EXE}", file=sys.stderr)
        return 2

    # Clean leftover screenshots so we know the run produced fresh ones.
    for stem in ("muncher_osc_before.png", "muncher_osc_after.png"):
        f = DEBUG_DIR / stem
        if f.exists():
            f.unlink()

    print(f"Spawning editor: {EDITOR_EXE}")
    proc = subprocess.Popen(
        [str(EDITOR_EXE), "--headless", "--script", str(SCRIPT_PATH)],
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    print("Waiting for editor exit...")
    try:
        out, _ = proc.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        print("FAIL: editor did not exit within timeout", file=sys.stderr)
        return 3

    # Save the log for grep + post-mortem.
    log_path = REPO_ROOT / "debug" / "muncher_osc_test.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(out)

    # --- assertions ------------------------------------------------------
    fail = False

    def must_contain(haystack: str, needle: str, label: str) -> None:
        nonlocal fail
        if needle not in haystack:
            print(f"FAIL: log missing '{label}': {needle!r}", file=sys.stderr)
            fail = True
        else:
            print(f"  ok: {label}")

    must_contain(out, "[Engine] Created Muncher generative layer",
                 "Muncher layer creation")

    before = DEBUG_DIR / "muncher_osc_before.png"
    after  = DEBUG_DIR / "muncher_osc_after.png"
    for f, label in ((before, "before-OSC screenshot"),
                     (after,  "after-OSC screenshot")):
        if not f.exists():
            print(f"FAIL: missing {label}: {f}", file=sys.stderr)
            fail = True
        else:
            size = f.stat().st_size
            print(f"  ok: {label} ({size} bytes)")

    if fail:
        print("\nFAIL — see debug/muncher_osc_test.log for full editor output",
              file=sys.stderr)
        return 1

    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
