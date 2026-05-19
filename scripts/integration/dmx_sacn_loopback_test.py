#!/usr/bin/env python3
"""Loopback integration test for the dmx-artnet plugin's sACN (E1.31) inbound.

Multicasts an sACN packet to 239.255.0.0:5568 (universe 0) and asserts the
plugin's mapping fires for the baked default mapping table.

Same hermetic-APPDATA pattern as the Art-Net loopback test, but with sACN
enabled in the temp settings.

Usage:
    python scripts/integration/dmx_sacn_loopback_test.py
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


def sacn_packet(universe: int, channels: list[int], priority: int = 100,
                sequence: int = 1) -> bytes:
    payload = bytearray(512)
    payload[:len(channels)] = bytes(channels)
    pkt = bytearray(638)
    pkt[0:2]     = b"\x00\x10"
    pkt[4:16]    = b"ASC-E1.17\x00\x00\x00"
    pkt[16:18]   = b"\x72\x6E"
    pkt[18:22]   = b"\x00\x00\x00\x04"
    pkt[38:40]   = b"\x72\x58"
    pkt[40:44]   = b"\x00\x00\x00\x02"
    pkt[108]     = priority & 0xFF
    pkt[111]     = sequence & 0xFF
    pkt[113]     = (universe >> 8) & 0xFF
    pkt[114]     = universe & 0xFF
    pkt[115:117] = b"\x72\x0B"
    pkt[117]     = 0x02
    pkt[118]     = 0xA1
    pkt[121:123] = b"\x00\x01"
    pkt[123:125] = b"\x02\x01"
    pkt[126:638] = bytes(payload)
    return bytes(pkt)


def main() -> int:
    if not EDITOR_EXE.exists():
        print(f"FAIL: editor binary missing: {EDITOR_EXE}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    scratch_appdata = REPO_ROOT / "build" / "test-tmp" / "dmx_sacn_appdata"
    (scratch_appdata / "Entity").mkdir(parents=True, exist_ok=True)
    (scratch_appdata / "Entity" / "settings.json").write_text(
        '{"version":1,"dmxArtnetEnabled":false,"dmxSacnEnabled":true,'
        '"oscReceiverEnabled":false}'
    )
    env["APPDATA"] = str(scratch_appdata)

    print(f"Spawning editor: {EDITOR_EXE}")
    proc = subprocess.Popen(
        [str(EDITOR_EXE), "--headless", "--script", str(SCRIPT_PATH)],
        cwd=str(REPO_ROOT), env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1,
    )

    time.sleep(1.5)

    # Loopback test: sACN listener binds to INADDR_ANY so it accepts
    # unicast on 127.0.0.1:5568 as well as the spec's
    # 239.255.U_HI.U_LO multicast. Sending unicast keeps the test
    # hermetic — no dependency on Windows multicast routing through
    # the loopback adapter, which is finicky in CI environments.
    # The multicast path itself is exercised in a separate manual
    # test against a real sACN source.
    print("Sending sACN traffic to 127.0.0.1:5568 (unicast) ...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.sendto(sacn_packet(0, [255] + [0] * 7, sequence=1),
                    ("127.0.0.1", 5568))
        print("  sent universe=0 ch1=255 priority=100 (expected: Play)")
        time.sleep(0.15)
        sock.sendto(sacn_packet(0, [0] * 8, sequence=2),
                    ("127.0.0.1", 5568))
        time.sleep(0.1)
        sock.sendto(sacn_packet(0, [0, 0, 0, 255, 0, 0, 0, 0], sequence=3),
                    ("127.0.0.1", 5568))
        print("  sent universe=0 ch4=255 (expected: SectionGo)")
    finally:
        sock.close()

    try:
        out, _ = proc.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        print("FAIL: editor did not exit within timeout", file=sys.stderr)
        return 3

    log_path = REPO_ROOT / "debug" / "dmx_sacn_loopback.log"
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

    must_contain("dmx-artnet: sACN listening on UDP 0.0.0.0:5568",
                 "plugin bound sACN listener")
    must_contain("fired Play",      "Play fired on ch1 edge")
    must_contain("fired SectionGo", "SectionGo fired on ch4 edge")

    if fail:
        print("\nFAIL -- see debug/dmx_sacn_loopback.log for full editor output",
              file=sys.stderr)
        return 1

    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
