# ACK and ACKACK semantics

This document fixes Robotweax SRT's interpretation of Full, Small, and Light
ACK numbering and the corresponding ACKACK/RTT state. It separates the
32-bit acknowledgement-number space from the 31-bit DATA packet-sequence
space.

## Selected specification behavior

The selected SRT Internet-Draft defines the Full ACK acknowledgement number as
an independent 32-bit sequential value starting at 1. An ACKACK copies that
number so the receiver can match the response to the Full ACK departure time.
Light and Small ACKs use zero in the type-specific field and do not take part
in the standard ACK/ACKACK RTT exchange.

The draft does not define the arithmetic after the largest 32-bit value.
Robotweax uses the complete non-zero field and wraps `UINT32_MAX` to 1. Zero
remains reserved for Light and Small ACK classification. This is an explicit,
tested interpretation of an underspecified rollover, not a DATA-sequence
rule.

Some deployed SRT 1.5.x peers emit a numbered 16-byte Small ACK and expect an
ACKACK. Robotweax accepts that exact shape through an isolated receive-side
compatibility path. It continues to emit standard zero-numbered Small ACKs and
rejects numbered Light ACKs.

## Wire and runtime requirements

| Requirement | Robotweax behavior |
| --- | --- |
| Full ACK numbering | Independent unsigned 32-bit values start at 1, use the high bit normally, skip zero, and wrap from `UINT32_MAX` to 1. |
| Light and Small ACK numbering | Robotweax emits zero. A numbered 16-byte Small ACK is accepted only as the deployed compatibility form; a numbered Light ACK is malformed. |
| ACKACK shape | ACKACK copies the non-zero ACK number into the type-specific header field. Its CIF is exactly one four-byte zero word (`00 00 00 00`). An empty CIF, nonzero CIF bytes, any other CIF size, or a zero ACK number is malformed. |
| RTT matching | One bounded tracker matches the complete ACK number. Slot collision evicts the older entry; unknown, evicted, duplicate, or zero ACKACK values do not update RTT. |
| Timing sample | A match is consumed once and yields a sample only when arrival is later than departure. Values above the public 32-bit microsecond range are clamped. |
| Field width | Full ACK parsing and encoding preserve all 32 acknowledgement-number bits and never apply DATA-sequence masking. |

### ACKACK control information field

Since Robotweax SRT 0.2.2, the zero word is required on the wire, not optional
padding. ACKACK therefore occupies 20 bytes: the 16-byte SRT control header
followed by the four-byte CIF. The acknowledgement number belongs in the
header's type-specific field, not in the CIF. A zero CIF is required; a zero
acknowledgement number is forbidden. This also applies to the receive-side
numbered Small ACK compatibility path described above.

The encoder supplies the zero word when an ACKACK packet description has no
payload. This encoding convenience does not make a received header-only
ACKACK valid. Noncanonical received controls are rejected before they can
affect RTT, statistics, liveness, or connection state. See the
[0.2.2 wire corrections](release-notes-0.2.2.md) and the corresponding
[PEERERROR contract](protocol-edge-cases.md#peererror).

## Bounded-state and rollover limit

Each connection stores 64 outstanding Full ACK departure records. The index is
derived from the acknowledgement number, but every lookup also compares the
complete 32-bit value. Reusing a slot therefore evicts the old record instead
of allowing a stale ACKACK to alias the new entry.

After an entire non-zero 32-bit cycle, the wire format contains no generation
field. A packet delayed across that complete cycle is indistinguishable from a
response to the newly reused number. Robotweax gives ownership to the current
active tracker entry. At the normal 10 ms Full ACK interval, a complete cycle
takes roughly 497 days; this protocol-level ambiguity is documented rather
than hidden by unbounded state or a non-interoperable wire extension.

## Sources

- Selected protocol description: `draft-sharabayko-srt-01`, sections 3.2.4,
  3.2.8, 4.8.1, and 4.10.
- De-facto compatibility behavior is scoped by
  [Compatibility status](compatibility.md).
