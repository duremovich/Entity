"""Tiny stdlib-only Art-Net sender for smoke-testing the dmx-artnet plugin.

Run after the editor is up with `dmxArtnetEnabled: true` in settings.json
(or ENTITY_DMX_ARTNET_PORT exported):

    python scripts/dmx_smoke_send.py [host] [port]

Defaults: 127.0.0.1 6454.
"""
from __future__ import annotations

import socket
import struct
import sys
import time


def artdmx(universe: int, channels: list[int], sequence: int = 1,
           physical: int = 0) -> bytes:
    """Build one ArtDmx packet for `universe` carrying `channels`."""
    if len(channels) > 512:
        raise ValueError("ArtDmx carries at most 512 channels")
    if len(channels) < 1:
        raise ValueError("ArtDmx must carry at least one channel")
    payload = bytes(channels)
    header = (
        b"Art-Net\x00"                              # 8 bytes magic
        + struct.pack("<H", 0x5000)                  # opcode ArtDmx LE
        + struct.pack(">H", 14)                      # protocol version
        + bytes([sequence & 0xFF, physical & 0xFF])  # sequence, physical
        + bytes([universe & 0xFF,                    # SubUni
                 (universe >> 8) & 0x7F])            # Net
        + struct.pack(">H", len(payload))            # length BE
    )
    return header + payload


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 6454
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    addr = (host, port)

    # Sequence designed to exercise the baked default mapping:
    #   ch 1 -> Play, ch 2 -> Pause, ch 4 -> SectionGo, ch 5 -> FireCue 1
    sequences = [
        ("Play",       [255] + [0] * 7),
        ("Pause",      [0, 255] + [0] * 6),
        ("SectionGo",  [0, 0, 0, 255] + [0] * 4),
        ("FireCue 1",  [0, 0, 0, 0, 255, 0, 0, 0]),
        ("FireCue 3",  [0, 0, 0, 0, 0, 0, 255, 0]),
    ]
    for label, channels in sequences:
        pkt = artdmx(universe=0, channels=channels)
        sock.sendto(pkt, addr)
        print(f"sent {label}: u=0 channels[1..8]={channels} ({len(pkt)} bytes)")
        time.sleep(0.1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
