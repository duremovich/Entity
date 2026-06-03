"""
signal_momentary_osc_check.py -- stdlib OSC listener for Phase 6 signal smoke.
Usage: python scripts/signal_momentary_osc_check.py [port]  (default: 53111)

Run this in one shell, then in another:
  EntityMediaEditor.exe --headless --script scripts/integration/signal_momentary_osc.json

Phase 6 smoke: expects exactly ONE /entity/signal packet on port 53111 within ~5 seconds.
Full assertion (comparing address/args) is deferred to Phase 8 when SetSignalAddress lands.

Exit codes:
  0 -- received at least one /entity/signal packet
  1 -- timeout (no packet within 5 seconds)
"""
import socket, struct, sys, time

PORT  = int(sys.argv[1]) if len(sys.argv) > 1 else 53111
TIMEOUT = 5.0

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", PORT))
sock.settimeout(TIMEOUT)
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
    return address, typetag


received = []
deadline = time.monotonic() + TIMEOUT

while time.monotonic() < deadline:
    remaining = deadline - time.monotonic()
    sock.settimeout(max(0.1, remaining))
    try:
        data, addr = sock.recvfrom(65535)
        try:
            address, typetag = decode_message(data)
            print(f"  OSC {address!r} {typetag}  from {addr[0]}:{addr[1]}", flush=True)
            received.append(address)
        except Exception as e:
            print(f"  [decode error: {e}]", flush=True)
    except socket.timeout:
        break

if received:
    print(f"OK: received {len(received)} OSC packet(s)", flush=True)
    sys.exit(0)
else:
    print("TIMEOUT: no OSC packets received", flush=True)
    sys.exit(1)
