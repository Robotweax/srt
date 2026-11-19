# SPDX-License-Identifier: MIT
"""Write deterministic, synthetic parser seeds; no operational credentials."""
import argparse
import struct
from pathlib import Path


def record(kind):
    size = {1: 136, 2: 232, 3: 104}[kind]
    data = bytearray(size)
    data[:8] = bytes([1, kind, 1, 0, 2, 16, 0, 0])
    if kind != 3:
        data[8:40] = bytes(range(32))
        data[40:104] = bytes(range(64))
        data[104:136] = bytes(range(32, 64))
    if kind != 1:
        offset = 136 if kind == 2 else 8
        data[offset:offset + 64] = bytes(range(64))
        data[offset + 64:offset + 96] = bytes(range(64, 96))
    return bytes(data)


def payload(kind):
    content = record(kind)
    header = struct.pack("!12I", 5, (2 << 16) | 4, 1000, 1500, 25600,
                         0xFFFFFFFE if kind == 3 else 0xFFFFFFFF,
                         456 if kind == 2 else 123, 0x12345678, 0, 0, 0, 0)
    return header + struct.pack("!HH", 0x7F16, len(content) // 4) + content


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    header = struct.pack("!4I", 0x80000000, 0, 77, 456)
    for kind in (1, 2, 3):
        body = payload(kind)
        (args.output / f"record-{kind}").write_bytes(record(kind))
        (args.output / f"datagram-{kind}").write_bytes(header + body)
        (args.output / f"opaque-{kind}").write_bytes(
            header + body + struct.pack("!HHI", 0x7F99, 1, 0x12345678))
    m1, m2, m3 = (payload(kind) for kind in (1, 2, 3))
    (args.output / "transcripts").write_bytes(struct.pack("!HH", len(m1), len(m2)) + m1 + m2 + m3)
    # Public standard P-256 generator followed by a deliberately invalid signature.
    generator = bytes.fromhex(
        "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
        "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5")
    (args.output / "point-and-signature").write_bytes(generator + bytes(64) + b"synthetic")
    print("wrote 11 deterministic peer-authentication seeds")


if __name__ == "__main__":
    main()
