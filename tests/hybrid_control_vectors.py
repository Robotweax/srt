#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent hashlib/hmac known answers for the staged v7 control component.

All inputs are public synthetic test fixtures. No socket or live key is used.
"""
import hashlib
import hmac
import json
import struct

p = struct.pack
dispatch = (p('!II', 0x52574D01, 0x40000001) + p('!BBH', 4, 0, 6000)
            + bytes([239, 255, 1, 1]) + bytes(12) + bytes(16)
            + p('!IQB', 0x7FFFFFFE, 0, 1) + bytes(3))
source = (p('!IBBHI', 0x52575301, 2, 4, 9000, 0x40000001)
          + bytes([192, 0, 2, 1]) + bytes(16))
offer = p('!IIIIIIQQQ', 1, 1, 31, 7, 256, 0, 0, 0, 0)
selected = p('!IIIIIIQQQ', 1, 2, 31, 7, 256, 0, 71, 97, 1234)
context = p('!I', 0x52574801) + dispatch + source + offer + selected
assert len(dispatch) == 60 and len(source) == 32 and len(context) == 192
label = b'RWSRT-SESSION-7\0'
salt = label + bytes(range(32)) + bytes(range(32, 64)) + p('!QQ', 107, 108) + context
root = hashlib.pbkdf2_hmac('sha1', b'pairwise-control-fixture-key', salt, 2048, 32)
def proof(role):
    return hmac.digest(root, label + p('!BBQH', 1, role, 0, 3) + bytes([1, 2, 3]), 'sha256').hex()
listener_key = hmac.digest(root, b'RWSRT-ACP1-1'.ljust(16, b'\0') + bytes([2, 1]), 'sha256')
body = p('!IIQQQQQIIQQQIIII', 1, 1, 71, 97, 1, 1, 1237, 1, 0, 1234, 1236, 1240, 1, 0, 1000, 0)
inner = p('!IIII', 0xFFFF7F16, 0, 77, 107) + body
wire = p('!IIII', 0xFFFF7F05, 0, 77, 107) + p('!IQ', 1, 1) + inner
tag = hmac.digest(listener_key, b'RWSRT-ACP1-MAC'.ljust(16, b'\0') + wire, 'sha256')
packet = wire + tag
assert len(packet) == 172
print(json.dumps({'context': context.hex(), 'caller_proof': proof(1), 'listener_proof': proof(2), 'packet': packet.hex()}, indent=2))
