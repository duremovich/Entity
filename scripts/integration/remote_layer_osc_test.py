#!/usr/bin/env python3
"""
End-to-end OSC integration test for the ADR-0028 remote-control plane.

Launches the headless editor with `remote_layer_osc.json`. The script patches
a Muncher layer to remote control, plays, and asserts render opacity at two
points. Sends are keyed off editor stdout (pumped on a reader thread), not
fixed sleeps:
  - Once the OSC listener binds: store opacity 0.25 while DISENGAGED. The
    script's first assert must still see 1.0 (stored value not applied).
  - Once the disengaged assert passes: send /entity/layer/muncher1/remote 1.0.
    The previously-stored 0.25 is applied WITHOUT re-sending opacity; the
    script's second assert must see 0.25.

This test exercises the store-while-disengaged contract: values written before
engage are held and applied on engage without a re-send.

Usage:
    python scripts/integration/remote_layer_osc_test.py

Returns nonzero on any unmet assertion.
"""

from __future__ import annotations

import os
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO_ROOT     = Path(__file__).resolve().parents[2]
EDITOR_EXE    = REPO_ROOT / "build" / "bin" / "Release" / "EntityMediaEditor.exe"
SCRIPT_PATH   = REPO_ROOT / "scripts" / "integration" / "remote_layer_osc.json"
DEBUG_DIR     = REPO_ROOT / "debug"
OSC_HOST      = "127.0.0.1"
OSC_PORT      = 53000


def osc_pad4(b: bytes) -> bytes:
    """Pad with NULs to the next 4-byte boundary, always appending at least one NUL."""
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


def send_f(sock, address: str, value: float) -> None:
    """Send a single-float OSC message."""
    pkt = osc_msg(address, float(value))
    sock.sendto(pkt, (OSC_HOST, OSC_PORT))
    print(f"  [osc-send] {address} {value!r}")


def main() -> int:
    if not EDITOR_EXE.exists():
        print(f"FAIL: editor binary missing: {EDITOR_EXE}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["ENTITY_FORCE_WARP"] = "1"

    print(f"Spawning editor: {EDITOR_EXE}")
    proc = subprocess.Popen(
        [str(EDITOR_EXE), "--headless", "--script", str(SCRIPT_PATH)],
        cwd=str(REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=env,
    )

    # Drain stdout from spawn on a reader thread. The editor writes >4KB of
    # startup logging BEFORE binding the OSC socket; with an undrained pipe
    # (4KB buffer on Windows) it blocks mid-startup until communicate()
    # finally reads, so fixed-sleep sends land before the listener exists.
    # Pumping continuously also lets us key each send off observed editor
    # state instead of guessing at sleeps.
    lines: list[str] = []
    lines_lock = threading.Lock()

    def pump() -> None:
        for line in proc.stdout:
            with lines_lock:
                lines.append(line)

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()

    def wait_for(needle: str, timeout_s: float, label: str) -> bool:
        deadline = time.monotonic() + timeout_s
        seen = 0
        while time.monotonic() < deadline:
            with lines_lock:
                chunk = lines[seen:]
                seen = len(lines)
            if any(needle in ln for ln in chunk):
                return True
            if proc.poll() is not None:
                return False
            time.sleep(0.05)
        print(f"FAIL: timed out waiting for '{label}'", file=sys.stderr)
        return False

    print("Sending OSC traffic...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ok_sequencing = True
    try:
        # Store opacity 0.25 while DISENGAGED, as soon as the listener is up
        # and well before the script's first assert (~3s of script time).
        if wait_for("osc-receiver listening", 15.0, "OSC listener bind"):
            send_f(sock, "/entity/layer/muncher1/opacity", 0.25)
        else:
            ok_sequencing = False
        # Engage only AFTER the disengaged assert has passed (it must see
        # 1.0), and within the script's second WaitSeconds(3.0) window.
        if ok_sequencing and wait_for("[AssertRemoteRenderOpacity] OK: muncher1 opacity 1",
                                      15.0, "disengaged assert"):
            send_f(sock, "/entity/layer/muncher1/remote", 1.0)
        else:
            ok_sequencing = False
    finally:
        sock.close()

    print("Waiting for editor exit...")
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        print("FAIL: editor did not exit within timeout", file=sys.stderr)
        return 3
    reader.join(timeout=5)
    with lines_lock:
        out = "".join(lines)
    # A sequencing timeout already printed its own FAIL line; the log
    # assertions below will also fail, which is what flips the exit code.

    # Save the log for post-mortem.
    log_path = DEBUG_DIR / "remote_layer_osc_test.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(out)

    # --- assertions ----------------------------------------------------------
    fail = False

    def must_contain(haystack: str, needle: str, label: str) -> None:
        nonlocal fail
        if needle not in haystack:
            print(f"FAIL: log missing '{label}': {needle!r}", file=sys.stderr)
            fail = True
        else:
            print(f"  ok: {label}")

    # Disengaged assert: opacity must be 1.0 (stored 0.25 not yet applied).
    must_contain(out, "[AssertRemoteRenderOpacity] OK: muncher1 opacity 1",
                 "disengaged opacity == 1.0")
    # Engaged assert: the stored 0.25 applied on engage without re-send.
    must_contain(out, "[AssertRemoteRenderOpacity] OK: muncher1 opacity 0.25",
                 "engaged opacity == 0.25 (store-while-disengaged contract)")
    # Muncher layer creation sanity.
    must_contain(out, "[Engine] Created Muncher generative layer",
                 "Muncher layer creation")

    if proc.returncode != 0:
        print(f"FAIL: editor exited with code {proc.returncode}", file=sys.stderr)
        fail = True
    else:
        print("  ok: editor exit code 0")

    if fail:
        print("\nFAIL -- see debug/remote_layer_osc_test.log for full editor output",
              file=sys.stderr)
        return 1

    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
