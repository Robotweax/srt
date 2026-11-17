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
result = {'context': context.hex(), 'caller_proof': proof(1), 'listener_proof': proof(2), 'packet': packet.hex()}

# Admission owner fixture. Independent exact payload construction, including
# SRT's reversed four-byte text words. No C++ codec or generated output is read.
def ext(kind, content):
    assert len(content) % 4 == 0
    return p('!HH', kind, len(content) // 4) + content

def handshake(sid, cookie):
    return p('!IIIIIIII4I', 5, 5, 0x7ffffffe, 1500, 25600, 0xffffffff,
             sid, cookie, 0, 0, 0, 0)

parameters = p('!IIHH', 0x010507, 0x3f, 120, 120)
text = b'live/camera1'
text += bytes(-len(text) % 4)
text = b''.join(text[i:i+4][::-1] for i in range(0, len(text), 4))
caller_payload = (handshake(107, 123) + ext(1, parameters) + ext(5, text)
                  + ext(8, p('!IBBH', 0x40000002, 4, 0, 0))
                  + ext(0x7f15, p('!IBBHI', 0x52575301, 1, 0, 0, 0) + bytes(20)))
listener_payload = (handshake(108, 0) + ext(2, parameters)
                   + ext(8, p('!IBBH', 0x40000001, 4, 0, 0))
                   + ext(0x7f14, dispatch) + ext(0x7f15, source))
admission = (p('!IIII', 0x52574101, 224 + len(caller_payload) + len(listener_payload),
               len(caller_payload), len(listener_payload)) + bytes([0x11])*16
             + context + caller_payload + listener_payload)
admission_root = hashlib.pbkdf2_hmac('sha1', bytes([0x22])*32, salt, 2048, 32)
def admission_proof(role):
    return hmac.digest(admission_root, label + p('!BBQH', 1, role, 0, len(admission)) + admission, 'sha256').hex()
result['admission'] = {'transcript': admission.hex(), 'size': len(admission),
                       'caller_proof': admission_proof(1), 'listener_proof': admission_proof(2)}
receipt_material = b'RWSRT-HYB-READY\0' + bytes.fromhex(admission_proof(1))
assert len(receipt_material) == 48
receipt_proof = hmac.digest(admission_root, label + p('!BBQH', 1, 2, 0, 48) + receipt_material, 'sha256')
def exchange_record(kind, capability, caller_nonce, listener_nonce, proof):
    record = (p('!IIII', 0x52575801, kind, 107, 108) + bytes([0x11])*16
              + capability + caller_nonce + listener_nonce + proof)
    assert len(record) == 176
    return record.hex()
cn, ln = bytes(range(32)), bytes(range(32, 64))
result['exchange'] = {
    'offer': exchange_record(1, offer, cn, bytes(32), bytes(32)),
    'selection': exchange_record(2, selected, cn, ln, bytes.fromhex(admission_proof(2))),
    'proof': exchange_record(3, bytes(48), cn, ln, bytes.fromhex(admission_proof(1))),
    'receipt': exchange_record(4, bytes(48), cn, ln, receipt_proof),
    'receipt_proof': receipt_proof.hex()}
# Negotiated policy_query uses kind7 and echoes the request in response word72.
query = p('!IIQQQQQIIQQQIIII', 1, 7, 71, 97, 1, 1, 1237, 1, 0, 0, 0, 0, 0, 0, 0, 0)
status = p('!IIQQQQQIIQQQIIII', 1, 5, 71, 97, 1, 1, 1237, 1, 0, 1234, 0, 0x0102030405060708, 0, 0, 0, 0)
assert len(query) == len(status) == 96
assert status[72:80] == bytes(range(1, 9))
result['policy_query'] = {'query': query.hex(), 'status_echo': status.hex(), 'feature': 32}
# return_control64: word64 is the effective boundary in delivery responses.
return_status = p('!IIQQQQQIIQQQIIII', 1, 5, 71, 97, 9, 2, 1242, 6, 2, 1234, 1237, 8, 0, 0, 0, 0)
ready_return = p('!IIQQQQQIIQQQIIII', 1, 6, 71, 97, 10, 2, 1241, 5, 0, 0, 1237, 0, 0, 0, 1000, 20)
assert len(return_status) == len(ready_return) == 96
assert return_status[64:72] == p('!Q', 1237) and return_status[72:80] == p('!Q', 8)
result['return_control'] = {'feature':64,'status_boundary':return_status.hex(),'ready_coverage':ready_return.hex()}
startup_offer = p('!IIIIIIQQQ', 1, 1, 167, 167, 256, 0, 0, 0, 0)
startup_selection = p('!IIIIIIQQQ', 1, 2, 167, 167, 256, 0, 71, 97, 1234)
assert len(startup_offer) == len(startup_selection) == 48
result['startup_fallback'] = {'feature': 128, 'offer': startup_offer.hex(), 'selection': startup_selection.hex()}
print(json.dumps(result, indent=2))
