"""
signal_continuous_osc_check.py -- listener for Phase 8 continuous signal test.
Usage: python scripts/signal_continuous_osc_check.py [port]  (default: 53111)

Run this before starting the editor:
  EntityMediaEditor.exe --headless --script scripts/integration/signal_continuous_osc.json

Expects /entity/signal/continuous int packets with values rising from ~0 to ~255
over the 100-frame play span. Asserts:
  - At least one packet received.
  - First value <= 10  (near start of span).
  - Last value  >= 245 (near end of span).

Exit codes:  0 = OK,  1 = FAIL or timeout.
"""
import socket, struct, sys, time

PORT    = int(sys.argv[1]) if len(sys.argv) > 1 else 53111
TIMEOUT = 6.0
TARGET  = "/entity/signal/continuous"

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", PORT))
print(f"Listening UDP 0.0.0.0:{PORT} (timeout {TIMEOUT}s)", flush=True)


def read_string(data, offset):
    end = data.index(b"\x00", offset)
    s   = data[offset:end].decode("ascii", "replace")
    return s, (end + 4) & ~3


def decode_message(data, offset=0):
    address, offset = read_string(data, offset)
    if offset >= len(data):
        return address, []
    typetag, offset = read_string(data, offset)
    args = []
    for t in typetag[1:]:  # skip leading ','
        if t == "i":
            val, = struct.unpack_from(">i", data, offset); offset += 4
            args.append(("int", val))
        elif t == "f":
            val, = struct.unpack_from(">f", data, offset); offset += 4
            args.append(("float", round(val, 4)))
        elif t == "s":
            s, offset = read_string(data, offset)
            args.append(("str", s))
    return address, args


def decode_bundle(data):
    offset, results = 16, []
    while offset + 4 <= len(data):
        n, = struct.unpack_from(">I", data, offset); offset += 4
        try:
            a, args = decode_message(data, offset)
            results.append((a, args))
        except Exception:
            pass
        offset += n
    return results


values = []
deadline = time.monotonic() + TIMEOUT

while time.monotonic() < deadline:
    remaining = deadline - time.monotonic()
    sock.settimeout(max(0.1, remaining))
    try:
        data, addr = sock.recvfrom(65535)
        msgs = decode_bundle(data) if data[:7] == b"#bundle" else []
        if not msgs:
            try:
                a, args = decode_message(data)
                msgs = [(a, args)]
            except Exception:
                pass
        for address, args in msgs:
            if address == TARGET and args:
                t, v = args[0]
                val = int(v) if t == "int" else int(float(v))
                print(f"  {address} {t}={val}  from {addr[0]}:{addr[1]}", flush=True)
                values.append(val)
    except socket.timeout:
        break

if not values:
    print("FAIL: no packets received", flush=True)
    sys.exit(1)

first, last = values[0], values[-1]
print(f"OK: received {len(values)} packets, first={first}, last={last}", flush=True)

failed = False
if first > 10:
    print(f"FAIL: first value {first} should be <= 10 (near start of span)", flush=True)
    failed = True
if last < 245:
    print(f"FAIL: last value {last} should be >= 245 (near end of span)", flush=True)
    failed = True

sys.exit(1 if failed else 0)
