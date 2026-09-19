# Sender-drop semantics under kernel shaping

This qualification starts at Robotweax SRT revision
`ce3d36f77c567b69c0429f7f8aea6d9717d91045`. It explains why
`pktSndDropTotal` can increase while every original packet still reaches the
peer and application.

## Public meaning

The v1.5.7-compatible `pktSndDrop*` fields combine two local sender actions:

- expiration of an application message with a finite TTL; and
- TLPKTDROP abandonment of unacknowledged packets after the sender deadline.

In the second case, “drop” means removal of the retained packet from the local
send/retransmission buffer. The sender gives up future retransmission and sends
a DROPREQ for the sequence range. The counter does not assert that the original
UDP datagram was never sent, was discarded by the kernel or network, or failed
to reach the receiving application.

For Live mode, the TLPKTDROP deadline is
`max(peer latency + sender drop delay, 1000 ms) + 20 ms`. With 120 ms peer
latency and the default zero sender-drop delay, the effective deadline is
therefore 1,020 ms. The configured latency is not itself the minimum sender
retention time.

## Qualified Linux observation

An opt-in diagnostic build recorded successful UDP submissions, cumulative ACK
progress and each TLPKTDROP removal using timestamps from one Linux monotonic
clock. The Network Lab used direct namespaces and a veth pair; only the sender
qdisc imposed the rate. It ran three 15.232-Mbit/s repetitions and one
one-variable 30.464-Mbit/s comparison against Haivision 1.5.7.

| Active rate | Repetition | UDP submissions | TLPKTDROP removals | All removed packets previously submitted | Unique received | Payload |
| --- | ---: | ---: | ---: | --- | ---: | --- |
| 15.232 Mbit/s | 1 | 2,400 | 596 | yes | 2,400 | complete |
| 15.232 Mbit/s | 2 | 2,400 | 577 | yes | 2,400 | complete |
| 15.232 Mbit/s | 3 | 2,400 | 591 | yes | 2,400 | complete |
| 30.464 Mbit/s | 1 | 2,400 | 0 | vacuous | 2,400 | complete |

The instrumented counts vary slightly from the surrounding plain runs (595,
592 and 582), as expected for an intrusive per-event file trace. The causal
boundary is unchanged: every traced removal occurs at an age of 1,020,031 to
1,023,528 microseconds, after a successful original UDP submission. The three
qdiscs report zero drops and zero requeues.

Immediately before the first removal, cumulative ACK progress leaves 596, 577
and 591 packets in flight respectively. TLPKTDROP removes exactly those old
unacknowledged retained copies in two or three scheduler polls. ACKs for
sequence numbers inside an abandoned range can then arrive as stale progress;
later ACKs drain the younger tail to zero. The peer nevertheless reports all
2,400 originals because the already submitted datagrams continue through the
qdisc after local retransmission responsibility has ended.

At 30.464 Mbit/s the qdisc backlog at recovery is smaller and cumulative ACK
progress clears the retained packets before they reach 1,020 ms. Sender
pressure remains observable, but the sender performs no TLPKTDROP abandonment.

## Diagnostic build

The diagnostic is disabled by default and changes no public ABI. Enable it only
for a Linux investigation:

```sh
cmake -S . -B build-drop-trace \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DROBOTWEAX_SRT_SENDER_DROP_TRACE=ON
cmake --build build-drop-trace --parallel
ROBOTWEAX_SRT_SENDER_DROP_TRACE=/tmp/sender-drop.jsonl APPLICATION
```

The file contains `udp-submit`, `ack` and `tlpktdrop` records. Per-event writes
are intrusive, so compare their mechanism with uninstrumented controls rather
than treating traced timing as a performance measurement. The trace never
contains application payload.

`benchmarks/analyze_sender_drop_trace.py RESULT_ROOT` validates the four-run
evidence and emits the packet/ACK mapping as JSON.
