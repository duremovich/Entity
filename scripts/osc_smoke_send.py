"""Tiny stdlib-only OSC 1.0 sender for smoke-testing the osc-receiver plugin.

Run after the editor is up:
    python scripts/osc_smoke_send.py [host] [port]

Defaults: 127.0.0.1 53000.
"""
from __future__ import annotations

import socket
import struct
import sys
import time


def _pad(b: bytes) -> bytes:
    n = len(b)
    return b + b"\x00" * ((4 - (n % 4)) % 4)


def osc_string(s: str) -> bytes:
    return _pad(s.encode("ascii") + b"\x00")


def osc_msg(addr: str, *args) -> bytes:
    typetag = ","
    arg_bytes = b""
    for a in args:
        if isinstance(a, int):
            typetag += "i"
            arg_bytes += struct.pack(">i", a)
        elif isinstance(a, float):
            typetag += "f"
            arg_bytes += struct.pack(">f", a)
        else:
            raise ValueError(f"unsupported OSC arg type: {type(a)!r}")
    return osc_string(addr) + osc_string(typetag) + arg_bytes


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 53000
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    addr = (host, port)

    sequence = [
        ("/entity/play", ()),
        ("/entity/pause", ()),
        ("/entity/section/next", ()),
        ("/entity/cue/1.5/go", ()),
        ("/entity/seek", (120,)),
        ("/entity/stop", ()),
        ("/entity/this/does/not/exist", ()),
    ]
    for path, args in sequence:
        pkt = osc_msg(path, *args)
        s.sendto(pkt, addr)
        print(f"sent {path} args={args!r} ({len(pkt)} bytes)")
        time.sleep(0.05)
    return 0


if __name__ == "__main__":
    sys.exit(main())
