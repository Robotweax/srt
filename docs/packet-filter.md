# Packet-filter FEC

Robotweax SRT provides a built-in, allocation-bounded Forward Error Correction
(FEC) packet filter for Live/Message transport. The filter is negotiated during
HSv5 establishment and remains separate from socket I/O, encryption, and ARQ
reliability policy.

## Configuration

Configure `SRTO_PACKETFILTER` before `srt_bind`, `srt_listen`, or
`srt_connect`. The supported grammar is:

For the fixed sensor profile, applications can instead set
`SRTO_TRANSTYPE=SRTT_SENSOR` on both endpoints before bind/connect. This
selects the same `fec-sensor-v1,cols:4,rows:1,arq:never` filter and its
low-latency Live/Message bundle in one call. An endpoint using the shorthand
can connect to one configured with the exact filter string. The sensor type
disables TSBPD, late-packet dropping, periodic NAK, and retransmission flags;
its FEC recovery remains active. Switch back with `SRTT_LIVE` or `SRTT_FILE`
before binding if the sensor filter is no longer wanted.

```c
SRT_TRANSTYPE type = SRTT_SENSOR;
srt_setsockflag(socket, SRTO_TRANSTYPE, &type, sizeof(type));
```

For general FEC configurations, use:

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
For GCM, the receiver authenticates original source DATA before admitting
its protected wire bytes to FEC state. It reconstructs missing protected DATA
and authenticates each reconstruction before reliability or application delivery.

With AES-GCM, parity covers `ciphertext || 16-byte authentication tag`.
Authentication completes before reconstructed plaintext is visible to
reliability or the application. Corrupt parity therefore produces a bounded
authentication failure rather than unauthenticated plaintext.

The payload ceiling depends on MSS, address family, encryption mode, and FEC
overhead. A 1,316-byte application message is a common seven-times-188-byte
MPEG-TS profile, not a protocol limit.

`SRTO_PAYLOADSIZE` is not negotiated. The receiver sizes its recovery buffers
from its own option and clips peer source and control payloads to that size,
as the reference implementation does: a peer with a smaller payload size is
zero padded and recovers normally; a peer with a larger payload size still has
its groups tracked, but a missing packet longer than the local buffer cannot
be rebuilt. With `arq:onreq`, such losses are reported only when the group
expires; Column and Matrix expiry requires subsequent packets to advance the
sequence. If the stream stops first, losses in its final groups can remain
unreported and unrecovered. `arq:always` uses ordinary loss reporting rather
than waiting for FEC expiry, but does not provide an unconditional guarantee
of recovery at stream end. Configure the same `SRTO_PAYLOADSIZE` on both peers
for full FEC recovery; use `arq:always` when retransmission should not depend on
FEC group expiry.

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

## Recovery after an unobserved Sensor window

An expired Sensor prefix may occupy every source position while its DATA and
initial retirement controls were lost. The sender can detach only contiguous
expired tombstones from the payload ring into constant-size retirement state,
admitting fresh samples without retaining or replaying the old payloads.
Outstanding detached retirement spans stay below a quarter sequence epoch.
ACK progress trims that state; local compaction is not peer acknowledgement.
Single-sequence DROPREQs remain the wire contract. Each retirement timer admits
at most 32 additional controls through the existing four-action and runtime
send budgets. Continued TTL expiry does not postpone an already armed repeat.
The receiver still limits every retirement to its observed physical DATA horizon.

Live/reliable DATA beyond the bounded receive window now exposes the missing
prefix within that window to ordinary loss reporting. A sender that abandoned
those sources can repeat their DROPREQ instead of leaving the receive window
stuck. No out-of-window DATA is delivered or falsely acknowledged.

Deterministic recovery tests include wraparound, a lost complete source window,
continuous Sensor TTL expiry with real runtime/FEC handling and an injected
clock, backpressure for unexpired sources, and retirement before any physical
DATA observation. This addresses [issue #198](https://github.com/Robotweax/srt/issues/198);
Broadcast Link's separate netem qualification must also pass before integration.

## Authenticated Sensor samples

`ENABLE_AEAD_API_PREVIEW=ON` permits explicit `SRTO_CRYPTOMODE=2` with
`SRTT_SENSOR` or the exact `fec-sensor-v1,cols:4,rows:1,arq:never` filter.
Selecting the transport and GCM in either order preserves the Sensor bundle:
Message API, no TSBPD, no late-packet dropping, no periodic NAK, and no new ARQ
obligation. A secret and enforced encryption are mandatory. Optional encryption
is an invalid combination; missing credentials or mode/filter mismatch fails
before application delivery. See the [GCM negotiation contract](aes-gcm-contract.md).
After establishment require mode `2`; after exchanging DATA, both
`SRTO_SNDKMSTATE` and `SRTO_RCVKMSTATE` must be `SRT_KM_S_SECURED`.

A Sensor message is one complete sample fitting the effective `SRTO_PAYLOADSIZE`.
It is not fragmented. With MSS 1500, unfiltered overhead plus FEC and the GCM tag
leaves at most 1436 plaintext bytes for IPv4 or 1416 for IPv6. Selecting
`SRTT_SENSOR` defaults to 1316 bytes, so a 1316-byte carrier fits either family.
The direct filter string retains the previously requested payload size, clipped
to the same ceiling. At IPv6 MSS 1280 the ceiling is 1196 bytes. Oversized sends
fail with `SRT_EINVOP` under the existing Sensor Message API error mapping;
set matching payload sizes on both peers for recovery.

Keep `inorder=0` for independent current samples. Finite `msgttl` is permitted;
expired sources are retired rather than retransmitted. One lost source in a
four-source row can be reconstructed with parity. Larger loss or unavailable
parity can lose samples: the existing 20-ms receive-gap deadline and bounded
retirement remain active. A read returns a complete available sample or the
usual timeout/nonblocking error, not a fabricated replacement. Receive order
need not match source order after FEC recovery. Use sequence/sample identities
when the application needs the newest value. Send success reports local buffering.

Original and reconstructed DATA authenticate before publication. Corrupted
source bytes cannot populate GCM FEC state; corrupt parity can prevent recovery
but cannot publish unauthenticated plaintext. `pktRcvUndecryptTotal` reports
rejected protected DATA/reconstruction; `pktRcvFilterSupplyTotal` counts unique
successfully admitted reconstructions. Filter extra/loss and receive-drop
counters describe recovery traffic and abandoned gaps; none promise loss-free
Sensor delivery. Duplicates do not generate another application receive. Rotation,
old receive slots and replay state remain bounded and connection-local.

The direct Message demo supports both directions without a consumer SDK:

```sh
# Supply the same SRT_SENSOR_SECRET in both shells.
robotweax_srt_message_demo listener --port 9001 --profile sensor \
  --crypto gcm --passphrase-env SRT_SENSOR_SECRET --nonblocking
robotweax_srt_message_demo caller --host 127.0.0.1 --port 9001 \
  --profile sensor --crypto gcm --passphrase-env SRT_SENSOR_SECRET \
  --message sample --nonblocking
```

The echo illustrates a healthy connection; Sensor does not guarantee command
acknowledgements. Transport ACK/NAK, key management, retirement and shutdown
controls are not all authenticated by DATA GCM. Unauthenticated parity and
transport controls retain denial-of-service limits. Fresh sockets have fresh
crypto/recovery state, without persistent replay or exactly-once execution history.
