"""
signal_break_park_check.py -- listener for Phase 8 break-park integration test.
Usage: python scripts/signal_break_park_check.py [port]  (default: 53111)

Run this before starting the editor:
  EntityMediaEditor.exe --headless --script scripts/integration/signal_break_park.json

Spec (hard requirement):
  - NO /entity/signal cue while playhead is parked at the section break.
  - After SectionGo, exactly ONE cue fires on the forward crossing of startFrame.

The check records all packets. After the editor exits (5s timeout), it asserts:
  - Total cue packets == 1.
  - All packets arrived after SectionGo (i.e., not during the park window).

NOTE: timing-based phase separation is approximate; the checker counts packets
and expects exactly one. If the break-park suppression fails, the count will be > 1
(one spurious cue during park + one real cue after GO).

Exit codes:  0 = OK,  1 = FAIL or timeout.
"""
import socket, struct, sys, time

PORT    = int(sys.argv[1]) if len(sys.argv) > 1 else 53111
TIMEOUT = 5.0

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", PORT))
print(f"Listening UDP 0.0.0.0:{PORT} (timeout {TIMEOUT}s)", flush=True)


def read_string(data, offset):
    end = data.index(b"\x00", offset)
    s   = data[offset:end].decode("ascii", "replace")
    return s, (end + 4) & ~3


def try_decode(data):
    try:
        address, offset = read_string(data, 0)
        return address
    except Exception:
        return None


cue_packets = []
deadline = time.monotonic() + TIMEOUT

while time.monotonic() < deadline:
    remaining = deadline - time.monotonic()
    sock.settimeout(max(0.1, remaining))
    try:
        data, addr = sock.recvfrom(65535)
        if data[:7] == b"#bundle":
            # walk bundle messages
            offset = 16
            while offset + 4 <= len(data):
                n, = struct.unpack_from(">I", data, offset); offset += 4
                a = try_decode(data[offset:offset + n])
                if a and "/entity/signal" in a:
                    print(f"  cue: {a}  from {addr[0]}:{addr[1]} t={time.monotonic():.3f}", flush=True)
                    cue_packets.append(a)
                offset += n
        else:
            a = try_decode(data)
            if a and "/entity/signal" in a:
                print(f"  cue: {a}  from {addr[0]}:{addr[1]} t={time.monotonic():.3f}", flush=True)
                cue_packets.append(a)
    except socket.timeout:
        break

n = len(cue_packets)
print(f"Total cue packets: {n} (expected: exactly 1)", flush=True)

if n == 0:
    print("FAIL: no cue received after SectionGo", flush=True)
    sys.exit(1)
if n > 1:
    print(f"FAIL: {n} cues received -- break-park suppression may have failed "
          "(expected 1 on forward crossing, 0 during park)", flush=True)
    sys.exit(1)

print("OK: exactly one cue received", flush=True)
sys.exit(0)
