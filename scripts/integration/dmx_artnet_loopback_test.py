#!/usr/bin/env python3
"""Loopback integration test for the dmx-artnet plugin's Art-Net inbound path.

Launches the headless editor with a small wait-and-exit script, sends a few
ArtDmx packets via UDP loopback, and asserts the plugin logged the expected
mapping fires for the baked default mapping table (universe 0 channels 1-8 =
transport + cues 1-4).

Editor settings prerequisite: the plugin must be enabled. The test exports
an env var the plugin reads instead of touching the user's settings.json.

Usage:
    python scripts/integration/dmx_artnet_loopback_test.py

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

REPO_ROOT   = Path(__file__).resolve().parents[2]
EDITOR_EXE  = REPO_ROOT / "build" / "bin" / "Release" / "EntityMediaEditor.exe"
SCRIPT_PATH = REPO_ROOT / "scripts" / "integration" / "dmx_artnet_loopback_wait.json"
DMX_HOST    = "127.0.0.1"
DMX_PORT    = 6454


def artdmx(universe: int, channels: list[int], sequence: int = 1) -> bytes:
    payload = bytes(channels)
    return (
        b"Art-Net\x00"
        + struct.pack("<H", 0x5000)
        + struct.pack(">H", 14)
        + bytes([sequence & 0xFF, 0])
        + bytes([universe & 0xFF, (universe >> 8) & 0x7F])
        + struct.pack(">H", len(payload))
        + payload
    )


def main() -> int:
    if not EDITOR_EXE.exists():
        print(f"FAIL: editor binary missing: {EDITOR_EXE}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    # Force the plugin on regardless of the user's settings.json.
    env["ENTITY_DMX_ARTNET_PORT"] = str(DMX_PORT)

    # The plugin's `dmxArtnetEnabled` setting is checked via
    # getBoolSetting before the port is read, so a fresh settings.json
    # would still keep the listener off. We write a temporary
    # settings.json in a scratch APPDATA dir so the test is hermetic.
    scratch_appdata = REPO_ROOT / "build" / "test-tmp" / "dmx_artnet_appdata"
    (scratch_appdata / "Entity").mkdir(parents=True, exist_ok=True)
    (scratch_appdata / "Entity" / "settings.json").write_text(
        '{"version":1,"dmxArtnetEnabled":true,"dmxArtnetListenPort":'
        + str(DMX_PORT) + ',"oscReceiverEnabled":false}'
    )
    env["APPDATA"] = str(scratch_appdata)

    print(f"Spawning editor: {EDITOR_EXE}")
    proc = subprocess.Popen(
        [str(EDITOR_EXE), "--headless", "--script", str(SCRIPT_PATH)],
        cwd=str(REPO_ROOT),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    # Wait for the UDP socket to bind. Plugin register happens early in
    # startup; 1.5s is plenty.
    time.sleep(1.5)

    print("Sending Art-Net traffic...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Bind sender to 127.0.0.1 explicitly. On Windows, some firewall
    # / Defender configurations drop UDP datagrams whose source isn't
    # a loopback address when the destination is 127.0.0.1; binding
    # forces the source to be 127.0.0.1 and avoids that drop.
    sock.bind(("127.0.0.1", 0))
    try:
        # Channel 1 -> Play (rising edge from 0 to 255).
        sock.sendto(artdmx(0, [255] + [0] * 7, sequence=1), (DMX_HOST, DMX_PORT))
        print("  sent ch1=255 (expected: Play)")
        time.sleep(0.15)
        # Reset channel 1 then trigger channel 4 -> SectionGo.
        sock.sendto(artdmx(0, [0] * 8, sequence=2), (DMX_HOST, DMX_PORT))
        time.sleep(0.1)
        sock.sendto(artdmx(0, [0, 0, 0, 255, 0, 0, 0, 0], sequence=3), (DMX_HOST, DMX_PORT))
        print("  sent ch4=255 (expected: SectionGo)")
        time.sleep(0.15)
        # Trigger channel 6 -> FireCue 2 via the ContiguousCueBlock mapping.
        sock.sendto(artdmx(0, [0, 0, 0, 0, 0, 255, 0, 0], sequence=4), (DMX_HOST, DMX_PORT))
        print("  sent ch6=255 (expected: FireCue 2)")
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

    log_path = REPO_ROOT / "debug" / "dmx_artnet_loopback.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(out)

    fail = False

    def must_contain(needle: str, label: str) -> None:
        nonlocal fail
        if needle not in out:
            print(f"FAIL: log missing '{label}': {needle!r}", file=sys.stderr)
            fail = True
        else:
            print(f"  ok: {label}")

    must_contain("dmx-artnet: Art-Net listening on UDP",
                 "plugin bound Art-Net listener")
    must_contain("fired Play", "Play fired on ch1 edge")
    must_contain("fired SectionGo", "SectionGo fired on ch4 edge")
    must_contain("fired FireCue", "FireCue fired on ch6 (ContiguousCueBlock)")

    if fail:
        print("\nFAIL -- see debug/dmx_artnet_loopback.log for full editor output",
              file=sys.stderr)
        return 1

    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
