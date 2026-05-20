"""
osc_smoke_listen.py — stdlib-only OSC 1.0 UDP listener.
Decodes int32/float32/string args and #bundle packets and pretty-prints them.
Usage: python osc_smoke_listen.py [port]   (default: 53001)
"""
import socket, struct, sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 53001
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", port))
print(f"Listening UDP 0.0.0.0:{port}", flush=True)


def read_string(data, offset):
    """Read null-terminated string, advance offset to next 4-byte boundary."""
    end = data.index(b"\x00", offset)
    s = data[offset:end].decode("ascii", "replace")
    aligned = (end + 4) & ~3
    return s, aligned


def decode_args(data, offset, typetag):
    """Decode OSC arguments from typetag string (after the leading ',')."""
    args = []
    for t in typetag:
        if t == "i":
            if offset + 4 > len(data):
                break
            val, = struct.unpack_from(">i", data, offset)
            args.append(("int32", val))
            offset += 4
        elif t == "f":
            if offset + 4 > len(data):
                break
            val, = struct.unpack_from(">f", data, offset)
            args.append(("float32", round(val, 6)))
            offset += 4
        elif t == "d":
            if offset + 8 > len(data):
                break
            val, = struct.unpack_from(">d", data, offset)
            args.append(("float64", val))
            offset += 8
        elif t == "h":
            if offset + 8 > len(data):
                break
            val, = struct.unpack_from(">q", data, offset)
            args.append(("int64", val))
            offset += 8
        elif t == "s":
            s, offset = read_string(data, offset)
            args.append(("string", s))
        else:
            args.append(("?", t))
    return args


def decode_message(data, offset=0):
    """Decode a single OSC message starting at offset. Returns (address, args)."""
    address, offset = read_string(data, offset)
    if offset >= len(data):
        return address, []
    typetag_str, offset = read_string(data, offset)
    if not typetag_str.startswith(","):
        return address, []
    args = decode_args(data, offset, typetag_str[1:])
    return address, args


def decode_bundle(data):
    """Decode an OSC #bundle. Returns list of (address, args) tuples."""
    # header: "#bundle\0" (8 bytes) + timetag (8 bytes)
    offset = 16
    results = []
    while offset + 4 <= len(data):
        elem_len, = struct.unpack_from(">I", data, offset)
        offset += 4
        elem = data[offset:offset + elem_len]
        offset += elem_len
        if elem[:7] == b"#bundle":
            results.extend(decode_bundle(elem))
        else:
            try:
                addr, args = decode_message(elem)
                results.append((addr, args))
            except Exception:
                pass
    return results


def format_args(args):
    return "  ".join(f"{t}={v}" for t, v in args) if args else "(no args)"


while True:
    try:
        data, addr = sock.recvfrom(65535)
    except KeyboardInterrupt:
        print("\nStopped.", flush=True)
        break
    try:
        if data[:7] == b"#bundle":
            messages = decode_bundle(data)
            print(f"[bundle from {addr[0]}:{addr[1]}  {len(messages)} msg(s)]", flush=True)
            for a, args in messages:
                print(f"  {a}  {format_args(args)}", flush=True)
        else:
            a, args = decode_message(data)
            print(f"{a}  {format_args(args)}  (from {addr[0]}:{addr[1]})", flush=True)
    except Exception as e:
        print(f"[decode error: {e}]", flush=True)
