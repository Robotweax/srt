# ARQ-first FEC and tail loss probes

ARQ-first FEC reduces repair traffic when retransmission can recover a lost
packet within the negotiated receive delay. It is an opt-in sender policy
for the existing matrix FEC filter. The optional tail loss probe helps
reveal losses at the end of a burst, when no later original packet would
otherwise expose a gap.

These options are development features and are not included in v0.3.1.

## Configuration

Before connecting, configure a matrix `fec` filter with `arq:always`, then
set `SRTO_ROBOTWEAX_FECPOLICY` to `ROBOTWEAX_SRT_FECPOLICY_ARQFIRST`.
Optionally enable `SRTO_ROBOTWEAX_TAILPROBE` after selecting that policy.
For example, the following configures an existing, unconnected socket:

```cpp
const char filter[] = "fec,cols:8,rows:8,layout:even,arq:always";
const int policy = ROBOTWEAX_SRT_FECPOLICY_ARQFIRST;
const bool probe = true;
if (srt_setsockflag(sock, SRTO_PACKETFILTER, filter,
        static_cast<int>(sizeof(filter) - 1)) == SRT_ERROR
    || srt_setsockflag(sock, SRTO_ROBOTWEAX_FECPOLICY, &policy,
        static_cast<int>(sizeof(policy))) == SRT_ERROR
    || srt_setsockflag(sock, SRTO_ROBOTWEAX_TAILPROBE, &probe,
        static_cast<int>(sizeof(probe))) == SRT_ERROR) {
    // Handle the option error before attempting to connect.
}
```

The policy requires Live transmission, TSBPD, the live congestion controller
and a matrix filter with `rows >= 2`. Groups, rendezvous, File and Control
modes, sensor filters and experimental `rwx-` filters are excluded. See
[Socket options](socket-options.md#arq-first-fec-and-the-tail-loss-probe-opt-in-sender-local)
for option types, defaults, setting order and validation rules.

The default policy is `ROBOTWEAX_SRT_FECPOLICY_ALWAYS` and the probe defaults
to false. Both settings are local to the sender. They add no filter-string
key or handshake field and require no new receiver feature beyond support
for the negotiated FEC filter.

## Application demos

The existing [Recovery application](../examples/applications/README.md#2-recovery-comparison),
[Message demo](../examples/README.md#arq-first-fec) and
[UDP-to-SRT bridge sender](udp-srt-bridge.md#arq-first-fec) accept
`--arq-first`. The switch supplies a compatible Matrix 8×8 filter and enables
ARQ-first FEC. In the Recovery and Message demos an explicitly configured
filter can override that geometry. The Recovery web console and comparison
launcher compare it with always-on FEC using identical geometry and
impairment settings. The demos do not expose a tail loss probe option.

## Recovery behavior

With `ALWAYS`, every ready FEC repair packet is sent. With `ARQFIRST`, a
ready repair is discarded when the sender's measured RTT and its variation
indicate that two retransmission rounds can fit the peer's receive delay.
Otherwise it is sent. Until an RTT measurement exists, full parity remains
enabled. The deadline formula is documented in
[Socket options](socket-options.md#arq-first-fec-and-the-tail-loss-probe-opt-in-sender-local).

When enabled, a tail probe queues a retransmission of the newest outstanding
sent packet. It retains the original sequence number, timestamp and payload
and uses the ordinary retransmitted flag. If earlier packets in the burst
are missing, receiving the probe allows the receiver to report those gaps.
Each idle period has at most three due times, including due times at which
no probe is sent. Only a newly submitted original starts a new idle period.

A probe interval is computed from the RTT estimate when the last original
is submitted, and subsequent intervals are computed when the previous due
time is evaluated. A later RTT update does not reschedule an existing due
time. Without an initial RTT measurement, the first interval is 120 ms,
even if a shorter RTT is measured before that interval ends. This can
prevent timely tail recovery on a short stream; the component study below
covered bursts that ended with a measured RTT already available.

Withheld repairs do not count as sent FEC packets. Queued probes use the
ordinary retransmission path. The internal counters `parity_withheld` and
`tail_probes_sent` are not exposed through `SRT_TRACEBSTATS`; see
[Statistics](statistics.md#packet-filter-and-fec-counters).

## Validation scope

A deterministic in-process component study compared 80 ARQ-first FEC cases
with 80 matching always-on Matrix FEC cases. The complete study contained
400 cases across five recovery configurations; it did not contain 400
ARQ-first FEC cases. The workload used Matrix 8×8 even geometry, `arq:always`,
a 120 ms negotiated receive delay, 1,250-byte sources at 1 ms intervals,
and a 20 Mbit/s simulated link, reduced to 12 Mbit/s for the capacity profile.

Across the 80 matched cases, aggregate IPv4 traffic in both directions was
123,762,844 bytes for ARQ-first FEC with the probe and 135,351,656 bytes for
always-on Matrix FEC, a reduction of 8.5620%. Neither configuration had
missing sources or deadline failures under the study's fixed 147 µs timing
tolerance. This result does not establish strictly zero lateness.

Integration unit tests cover option defaults and rejection rules, parity
decisions, probe scheduling, retransmission metadata and unchanged session
packet sequences with the options off. Loopback smoke tests also exercise
ARQ-first in the Recovery sender with CTR/GCM, Message Caller/Listener echo,
and UDP-to-SRT bridging in both connection directions, including AES-CTR.
They check configuration and payload integrity, not comparative performance.
The component-study results are not socket, scheduler, CPU or WAN measurements
and do not qualify other payloads, geometries, delays or encrypted operation.
