# Packet-filter FEC

Robotweax SRT provides a built-in, allocation-bounded Forward Error Correction
(FEC) packet filter for Live/Message transport. The filter is negotiated during
HSv5 establishment and remains separate from socket I/O, encryption, and ARQ
reliability policy.

## Configuration

Configure `SRTO_PACKETFILTER` before `srt_bind`, `srt_listen`, or
`srt_connect`. The supported grammar is:

```text
fec,cols:N[,rows:N][,layout:even|staircase][,arq:never|onreq|always]
```

| Parameter | Meaning |
| --- | --- |
| `cols` | Sources per row; Robotweax accepts 2 through 128 |
| `rows` | `1` for Row FEC, a positive value greater than 1 for Matrix FEC, or a value below `-1` for Column-only FEC |
| `layout` | Column placement: `even` or `staircase`; default `staircase` |
| `arq` | Interaction with retransmission: `never`, `onreq`, or `always`; default `onreq` |

Configuration strings are limited to 512 bytes. Invalid syntax, unsupported
filter names, incompatible peer parameters, over-capacity geometry, and option
changes after establishment fail explicitly.

A Caller request drives negotiation. A Listener validates compatible requested
parameters but does not force an unrequested filter. Rendezvous peers require
the same effective filter configuration. Negotiation failure uses
`SRT_REJ_FILTER`.

## Geometry and recovery

- **Row FEC** emits XOR parity for each `cols`-packet row and can directly
  recover one missing source per row.
- **Column FEC** maps sources into columns across the absolute row count and
  emits parity for each column.
- **Matrix FEC** emits both Row and Column parity. A packet recovered in one
  dimension is supplied to the other, allowing bounded iterative recovery of
  some multi-loss patterns.

Recovery is sequence-rollover safe. Source and parity may arrive in either
order. Duplicate source and control packets do not enlarge decoder state.
Irrecoverable losses are sorted and coalesced before they are handed to the
normal reliability path.

The `arq` policy controls that handoff:

- `always` reports physical losses through ordinary NAK/ARQ processing;
- `never` suppresses retransmission requests for the filtered flow;
- `onreq` reports only losses finalized as unrecoverable by the active decoder.

Matrix `onreq` finalization follows Column expiry because a missing Row source
may still be reconstructed vertically. If the requested decoder cannot be
installed safely, `onreq` falls back to `always` rather than suppressing loss
recovery.

## Payload and encryption order

FEC reserves four bytes from the maximum application payload. The sender
encrypts or authenticates DATA before generating parity, so FEC protects the
bytes carried on the wire. A control packet is not encrypted a second time.
The receiver reconstructs protected DATA first and then invokes the normal
decrypt or authenticate path.

With AES-GCM, parity covers `ciphertext || 16-byte authentication tag`.
Authentication completes before reconstructed plaintext is visible to
reliability or the application. Corrupt parity therefore produces a bounded
authentication failure rather than unauthenticated plaintext.

The payload ceiling depends on MSS, address family, encryption mode, and FEC
overhead. A 1,316-byte application message is a common seven-times-188-byte
MPEG-TS profile, not a protocol limit.

## Resource model

Decoder storage is determined at construction by the local receive-window
capacity `W`, maximum protected payload `P`, and accepted geometry. Remote DATA
or control packets cannot resize it.

For `C` columns and `R` absolute rows, the bounded group counts are:

```text
Row:     ceil(W / C) + 3
Column:  ceil(W / R) + 3 * C
Matrix:  Row + Column
```

Each group owns fixed metadata, a `P`-byte XOR area, and a source-presence
bitmap. Column and Matrix modes add bounded loss and reconstructed-packet
storage. Resource arithmetic is checked before allocation. Applications that
select large receive windows or custom matrices must account for the resulting
memory and recovery latency.

Haivision SRT v1.5.7 accepts a broader 16-bit row/column option range.
Robotweax does not mirror that range blindly: the 128-column ceiling prevents
remote configuration from driving disproportionate decoder group, bitmap,
XOR-area, and reconstruction storage. This is a deliberate resource-protection
boundary and a documented limit on complete FEC option-range parity, not a
wire-format difference for the qualified common geometries.

## Statistics

The public statistics surface provides packet counts for:

- sent parity packets (`pktSndFilterExtra*`);
- received FEC control packets (`pktRcvFilterExtra*`);
- reconstructed source packets (`pktRcvFilterSupply*`); and
- source packets declared irrecoverable (`pktRcvFilterLoss*`).

Each family has a cumulative `Total` field and an interval field without that
suffix. There are **no separate public filter-byte counters**.

Parity traffic contributes to the general sent/received packet and byte
counters, not to unique source traffic. A newly accepted reconstructed source
contributes to unique receive counters without another physical DATA receive.
See [Statistics](statistics.md#packet-filter-and-fec-counters) for byte
accounting, interpretation limits, and snapshot/clear behavior.

## Compatibility and limitations

Robotweax supports Row, Column, and Matrix FEC with `even` and `staircase`
placement for the documented Live/Message profiles, including clear,
AES-CTR, and supported Robotweax AES-GCM operation. Exact interoperability
depends on peer version, direction, geometry, encryption profile, and ARQ mode.

- File/Stream plus packet-filter FEC is rejected.
- Arbitrary packet-filter plugins are not supported.
- Row FEC alone cannot recover multiple missing sources in one row.
- FEC cannot recover every burst pattern; ARQ policy determines how finalized
  losses proceed to retransmission.
- Large geometries and sustained adversarial loss require deployment-specific
  memory, CPU, and timing qualification.

See [Compatibility status](compatibility.md),
[Encryption](encryption.md), and [Known limitations](limitations.md) before
combining FEC with other transport features.
