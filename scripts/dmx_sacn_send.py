"""Stdlib-only sACN (E1.31) sender for testing the dmx-artnet plugin's
sACN listener. Multicasts to 239.255.<hi>.<lo> on UDP 5568.

Run while the editor is up with `dmxSacnEnabled: true`:

    python scripts/dmx_sacn_send.py [universe] [priority]

Defaults: universe 0, priority 100.
"""
from __future__ import annotations

import socket
import struct
import sys
import time


def sacn_packet(universe: int, channels: list[int], priority: int = 100,
                sequence: int = 1) -> bytes:
    """Build one E1.31 DMX-data packet. Uses zero-CID + zero source name
    for simplicity — receivers tolerate this for test traffic."""
    if len(channels) > 512:
        raise ValueError("E1.31 carries at most 512 channels")
    if len(channels) < 1:
        raise ValueError("E1.31 must carry at least one channel")

    payload = bytearray(512)
    payload[:len(channels)] = bytes(channels)

    # Build the 638-byte fixed-size packet.
    pkt = bytearray(638)
    # --- Root layer ---
    pkt[0:2]   = b"\x00\x10"                          # preamble
    pkt[2:4]   = b"\x00\x00"                          # postamble
    pkt[4:16]  = b"ASC-E1.17\x00\x00\x00"             # packet identifier
    pkt[16:18] = b"\x72\x6E"                          # root flags+length
    pkt[18:22] = b"\x00\x00\x00\x04"                  # vector = E1.31 data
    # pkt[22:38] = CID (16 bytes, zeroed for test sender)
    # --- Framing layer ---
    pkt[38:40] = b"\x72\x58"                          # flags+length
    pkt[40:44] = b"\x00\x00\x00\x02"                  # vector = DMP data
    # pkt[44:108] = source name (64 bytes, zeroed)
    pkt[108]   = priority & 0xFF                       # priority
    pkt[109:111] = b"\x00\x00"                        # sync universe
    pkt[111]   = sequence & 0xFF                       # sequence
    pkt[112]   = 0                                     # options
    pkt[113]   = (universe >> 8) & 0xFF                # universe BE
    pkt[114]   = universe & 0xFF
    # --- DMP layer ---
    pkt[115:117] = b"\x72\x0B"                        # flags+length
    pkt[117]   = 0x02                                  # vector = set property
    pkt[118]   = 0xA1                                  # address/data type
    pkt[119:121] = b"\x00\x00"                        # first property address
    pkt[121:123] = b"\x00\x01"                        # address increment
    pkt[123:125] = b"\x02\x01"                        # property value count = 513
    pkt[125]   = 0x00                                  # DMX start code
    pkt[126:638] = bytes(payload)
    return bytes(pkt)


def main() -> int:
    universe = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    priority = int(sys.argv[2]) if len(sys.argv) > 2 else 100

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    multicast_host = f"239.255.{(universe >> 8) & 0xFF}.{universe & 0xFF}"
    addr = (multicast_host, 5568)

    sequence = [
        ("Play",      [255] + [0] * 7),
        ("Pause",     [0, 255] + [0] * 6),
        ("SectionGo", [0, 0, 0, 255] + [0] * 4),
        ("FireCue 2", [0, 0, 0, 0, 0, 255, 0, 0]),
    ]
    seq = 1
    for label, channels in sequence:
        pkt = sacn_packet(universe, channels, priority=priority, sequence=seq)
        sock.sendto(pkt, addr)
        print(f"sent {label}: u={universe} priority={priority} channels[1..8]={channels}")
        seq = (seq + 1) & 0xFF
        time.sleep(0.1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
