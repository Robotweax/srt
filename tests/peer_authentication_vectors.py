# SPDX-License-Identifier: MIT
"""Regenerate the synthetic fixtures in test_peer_authentication.cpp.

Uses only Python's standard library and independently constructs wire bytes.
The synthetic key shares and signatures are not valid P-256 test vectors.
"""

import hashlib
import hmac
import struct

LABEL = b"RWSRT-PEERAUTH-1"


def hmac256(key: bytes, message: bytes) -> bytes:
    return hmac.digest(key, message, "sha256")


def message(kind: int) -> bytes:
    size = {1: 136, 2: 232, 3: 104}[kind]
    content = bytearray(size)
    content[:8] = bytes([1, kind, 1, 0, 2, 16, 0, 0])
    if kind != 3:
        content[8:40] = b"\x01" * 32
        content[40:104] = b"\x02" * 64
        content[104:136] = b"\x03" * 32
    if kind != 1:
        offset = 136 if kind == 2 else 8
        content[offset : offset + 64] = b"\x04" * 64
        content[offset + 64 : offset + 96] = b"\x05" * 32
    header = struct.pack(
        "!12I", 5, 4, 0, 1500, 25600,
        0xFFFFFFFE if kind == 3 else 0xFFFFFFFF, 0, 0, 0, 0, 0, 0,
    )
    return header + struct.pack("!HH", 0x7F16, size // 4) + content


def frame(data: bytes) -> bytes:
    return struct.pack("!I", len(data)) + data


def expand(root: bytes, number: int, transcript_hash: bytes) -> bytes:
    return hmac256(root, LABEL + bytes([number]) + transcript_hash + b"\x01")


def main() -> None:
    m1, m2, m3 = (message(kind) for kind in (1, 2, 3))
    m2_zero = m2[:-96] + bytes(96)
    m3_zero = m3[:-96] + bytes(96)
    th_l = hashlib.sha256(LABEL + frame(m1) + frame(m2_zero)).digest()
    th_c = hashlib.sha256(LABEL + frame(m1) + frame(m2) + frame(m3_zero)).digest()
    hs = hmac256(bytes(32), bytes(range(32)))
    hkey1, hkey2 = (expand(hs, number, th_l) for number in (1, 2))
    psk = hashlib.pbkdf2_hmac(
        "sha1", b"fixture passphrase", LABEL + b"\x01" * 32 + b"\x02" * 32,
        2048, 32,
    )
    master = hmac256(hkey2, psk)
    vectors = [
        ("th_L", th_l),
        ("th_C", th_c),
        ("hkey2", hkey2),
        ("confirm_L", hmac256(hkey1, th_l + b"\x04" * 64)),
        ("confirm_C", hmac256(expand(master, 3, th_c), th_c + b"\x04" * 64)),
    ]
    vectors.extend((f"key{number}", expand(master, number, th_c)) for number in (4, 5, 6))
    for name, value in vectors:
        print(name, value.hex())


if __name__ == "__main__":
    main()
