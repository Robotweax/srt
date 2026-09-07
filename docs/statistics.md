# Statistics

Robotweax SRT exposes the selected Haivision SRT v1.5.7 statistics ABI with
Robotweax-owned, bounded runtime accounting.

- `CBytePerfMon` and `SRT_TRACEBSTATS` use the compatible 496-byte layout on
  supported 64-bit ABIs.
- `srt_bstats` returns cumulative totals, interval counters, and moving-average
  transport-buffer state.
- `srt_bistats` selects moving-average or instantaneous buffer state through
  its compatible selector.
- `srt_getsndbuffer` reports current packet blocks, payload bytes, and buffered
  timestamp span in milliseconds. Either output pointer may be null.

## Snapshot and clear behavior

Every event updates a cumulative and an interval counter. A call with
`clear != 0` first returns one coherent snapshot, then clears only interval
counters and their sample origin. Lifetime totals remain unchanged.

Counters use saturated arithmetic. Collecting a snapshot does not resize
transport state or add packet-path allocation.

## Packet and byte accounting

DATA wire-byte fields include:

- the 16-byte SRT DATA header;
- 8 bytes of UDP; and
- 20 bytes for IPv4 or 40 bytes for IPv6.

The resulting per-DATA overhead is 44 bytes for IPv4 and 64 bytes for IPv6.
IPv4-mapped destinations on a dual-stack socket use IPv4 accounting.

Undecryptable byte fields count protected payload bytes only. ACK and NAK
control traffic uses its dedicated packet counters rather than DATA byte
counters. One compressed multi-range NAK increments the NAK counter once,
regardless of the number of encoded ranges.

`byteMSS` reports the lower MSS committed by the handshake.

## Unique traffic and retransmission

Unique send counters advance on the first successful transmission. Retransmit
counters advance only when a retransmission datagram is actually sent.

Unique receive counters advance only when a DATA packet is newly accepted into
the receive window. A physical duplicate may increase total received traffic
without increasing unique traffic.

The sender relationships, including FEC parity when enabled, are:

```text
total sent packets = unique sent packets + retransmitted packets + sent parity packets
total sent wire bytes = unique sent wire bytes + retransmitted wire bytes + sent parity wire bytes
```

The parity terms are zero without FEC. Sent parity packets are exposed as
`pktSndFilterExtra*`; the public API has no separate parity-byte field.

Receiver loss is recorded from newly observed gaps relative to the highest
physical sequence. An open gap is not counted again for each later packet.
Sender loss is recorded when a NAK newly queues a packet for retransmission.
Flight-tail loss recovered solely by sender RTO need not create a receiver NAK
or receiver loss observation.

## Reorder tolerance

An original DATA packet that arrives below the highest physical sequence and
does not carry the retransmission flag is reorder evidence. The current
tolerance starts at zero and may grow up to `SRTO_LOSSMAXTTL`.

Changing the configured maximum behaves as follows:

- reducing it clamps the current tolerance;
- increasing it does not invent a new reorder observation; and
- ordered or retransmitted traffic gradually decays the current value.

Fresh gaps use the current tolerance as a packet-count delay before their first
NAK. `pktReorderTolerance` reports the current estimator, not merely the
configured maximum.

## Belated packets

A packet received after its delivery position has already expired increments
the belated count. The average belated delay is an exponentially smoothed
value. A late duplicate can increment physical traffic and belated statistics
without incrementing unique receive traffic.

Applications should compare belated, total, unique, retransmission, and loss
counters together. One counter alone does not identify the cause of late
arrival.

## Buffer state

Sender and receiver queue state use fixed-size samplers. Moving averages are
time-weighted over a one-second window; a long sampling gap restarts the
average from current state.

- `srt_bstats` and `srt_bistats(..., instantaneous = 0)` report moving values.
- `srt_bistats(..., instantaneous = 1)` reports current queue state.
- sender buffer bytes include DATA wire overhead;
- receiver buffer bytes report retained application payload; and
- a nonempty one-packet send buffer reports at least 1 ms of buffered time.

## Drop counters

Finite per-message TTL and negotiated TLPKTDROP are distinct protocol causes.
The v1.5.7 public layout has no separate fields, so `pktSndDrop*` and
`byteSndDrop*` report their combined result.

Sender and receiver drop counters describe local action. They are not proof
that a peer observed the same event.

## Packet-filter and FEC counters

| Counter family | Meaning |
| --- | --- |
| `pktSndFilterExtra*` | Row or Column parity packets sent; Matrix includes both dimensions |
| `pktRcvFilterExtra*` | Negotiated FEC control packets consumed by the receive seam |
| `pktRcvFilterSupply*` | Source packets reconstructed by Row, Column, or recursive Matrix recovery |
| `pktRcvFilterLoss*` | Source packets declared irrecoverable by `arq:onreq` at bounded group expiry |

These are packet counts, not byte counts. Each family has a cumulative `Total`
field and an interval field without that suffix, with the snapshot/clear rules
described above. `CBytePerfMon` / `SRT_TRACEBSTATS` has no corresponding
`byte*FilterExtra`, `byte*FilterSupply`, or `byte*FilterLoss` fields.

Successfully sent parity packets contribute to `pktSent*` and `byteSent*` but
not to the unique-send fields. Consumed FEC control packets contribute to
`pktRecv*` and `byteRecv*` but not to the unique-receive fields. Their byte
accounting includes the FEC control payload and the DATA/UDP/IP header overhead
described above; FEC control is carried in DATA-format datagrams.

A reconstructed source can increment unique receive without incrementing
physical DATA receive: a newly accepted source increases `pktRecvUnique*` and
`byteRecvUnique*`, but reconstruction alone does not increase `pktRecv*` or
`byteRecv*`. The unique byte accounting adds the same per-packet header allowance
to the accepted source payload; it is not evidence of another datagram on the
wire. Do not infer received parity bytes by subtracting unique receive bytes
from total receive bytes, or multiply filter packet counts by a fixed payload
size to obtain an exact byte count.

Matrix mode reports a final loss only after the packet can no longer be
recovered by either dimension. Re-reporting the same expired gap does not
increment loss again.

## Encryption counters

Undecryptable packet and byte counters include protected DATA that fails the
negotiated decrypt/authenticate boundary. Authentication failure does not
increment unique receive, delivery, FEC supply, or application-byte success
counters.

Key rotation itself does not reset traffic statistics. Clear interval counters
explicitly through the snapshot API when measuring a new phase.

## Interpretation limits

Statistics are a compatible observation surface, not a stable representation
of internal classes or scheduler decisions. Applications should:

- prefer counter relationships over one absolute field;
- account for interval-versus-lifetime semantics;
- record address family, MSS, mode, encryption, FEC, and clear flag with a
  measurement;
- avoid comparing snapshots taken at different lifecycle points; and
- treat fields outside the documented 0.2 implementation boundary as
  unavailable rather than inferred.

See [Public API compatibility](api-compatibility.md),
[Packet filters and FEC](packet-filter.md), and
[Known limitations](limitations.md).
