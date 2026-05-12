#!/usr/bin/env python3
"""
End-to-end OSC integration test for the Muncher generative layer.

Launches the headless editor with `muncher_osc_wait.json` and, while it
is alive and playing, sends `/entity/muncher/right` followed by
`/entity/muncher/down` to the editor's OSC receiver. Verifies:

 1. The OSC plugin dispatches `SetInputChannel` commands for the
    Muncher input axes.
 2. Two compose-target screenshots land on disk (before / after the
    OSC traffic). A visual eyeball confirms the player + pellet trail
    move in response to the OSC input.

Usage:
    python scripts/integration/muncher_osc_test.py

Returns nonzero on any unmet assertion.
"""

from __future__ import annotations

import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT     = Path(__file__).resolve().parents[2]
EDITOR_EXE    = REPO_ROOT / "build" / "bin" / "Release" / "EntityMediaEditor.exe"
SCRIPT_PATH   = REPO_ROOT / "scripts" / "integration" / "muncher_osc_wait.json"
DEBUG_DIR     = REPO_ROOT / "debug"
OSC_HOST      = "127.0.0.1"
OSC_PORT      = 53000


def osc_pad4(b: bytes) -> bytes:
    """Pad with NULs to the next 4-byte boundary, including a trailing NUL."""
    b = b + b"\x00"
    while len(b) % 4 != 0:
        b += b"\x00"
    return b


def osc_msg(address: str, *args) -> bytes:
    """Build a tiny OSC 1.0 message. Supports float and int args."""
    out = osc_pad4(address.encode("ascii"))
    type_tag = b","
    arg_blob = b""
    for a in args:
        if isinstance(a, float):
            type_tag += b"f"
            arg_blob += struct.pack(">f", a)
        elif isinstance(a, int):
            type_tag += b"i"
            arg_blob += struct.pack(">i", a)
        else:
            raise TypeError(f"unsupported OSC arg type: {type(a)}")
    out += osc_pad4(type_tag)
    out += arg_blob
    return out


def send(sock, address: str, *args) -> None:
    pkt = osc_msg(address, *args)
    sock.sendto(pkt, (OSC_HOST, OSC_PORT))
    print(f"  [osc-send] {address} {args}")


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

    # Wait for OSC listener to come up. We can't easily detect "ready"
    # from stdout in real time without a reader thread; the editor binds
    # the UDP socket within ~1s of launch, so sleep a hair longer than
    # the first WaitSeconds in muncher_osc_wait.json (1.5s).
    time.sleep(2.0)

    print("Sending OSC traffic...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # Player heads right for ~1.2s.
        send(sock, "/entity/muncher/right")
        time.sleep(1.2)
        # Then down for ~1.2s.
        send(sock, "/entity/muncher/down")
        time.sleep(1.2)
        # Stop so the player parks for the final screenshot.
        send(sock, "/entity/muncher/stop")
    finally:
        sock.close()

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

    must_contain(out, "[SetInputChannel] muncher.input.x = 1",
                 "OSC /entity/muncher/right -&gt; SetInputChannel x=1")
    must_contain(out, "[SetInputChannel] muncher.input.y = 1",
                 "OSC /entity/muncher/down -&gt; SetInputChannel y=1")
    must_contain(out, "[SetInputChannel] muncher.input.y = 0",
                 "OSC /entity/muncher/stop -&gt; SetInputChannel y=0")
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
