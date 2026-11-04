# ARQ-first FEC and tail loss probes

ARQ-first FEC reduces repair traffic when retransmission can recover a lost
packet within the negotiated receive delay. It is an opt-in sender policy
for the existing matrix FEC filter. The optional tail loss probe helps
reveal losses at the end of a burst, when no later original packet would
otherwise expose a gap.

Available from Robotweax SRT 0.3.2. ARQ-first is a regular opt-in sender
policy; existing applications retain always-on parity and no tail probes
unless they explicitly enable these options.

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
prevent timely tail recovery on a short stream. Without measured RTT feedback,
parity remains enabled and the probe stays disarmed. A short, incomplete FEC
matrix can therefore still lose its final packets; enabling the probe does
not establish a deadline guarantee for that cold-start case.

Withheld repairs do not count as sent FEC packets. Queued probes use the
ordinary retransmission path. The internal counters `parity_withheld` and
`tail_probes_sent` are not exposed through `SRT_TRACEBSTATS`; see
[Statistics](statistics.md#packet-filter-and-fec-counters).

## Validation scope

Integration unit tests cover option defaults and rejection rules, parity
decisions, probe scheduling, retransmission metadata and unchanged session
packet sequences with the options off. Loopback smoke tests also exercise
ARQ-first in the Recovery sender with CTR/GCM, Message Caller/Listener echo,
and UDP-to-SRT bridging in both connection directions, including AES-CTR.
They check configuration and payload integrity.

The `robotweax_srt_arq_transport_tests` CTest partition runs real UDP sockets,
the transport scheduler and public send/receive APIs with controlled packet
loss. With measured RTT, Matrix 8×8, 400 ms receive delay and a partial
three-message tail, it drops the last one or two originals and checks exact
ordered recovery, zero source drops and repair submission within the configured
delay. The cases cover plaintext, AES-CTR and AES-GCM when enabled. Separate
cases keep the probe off unless requested and verify the disarmed state with
missing RTT feedback at 60 ms and 400 ms receive delays.

```sh
ctest --test-dir build -R robotweax_srt_arq_transport_tests --output-on-failure
```

These are functional regression profiles, not CPU, throughput or WAN benchmarks.
Earlier numerical traffic-reduction results came from an external component
study whose complete pinned harness and raw evidence are not shipped here.
They are not used as a release performance claim. Other workloads, geometries
and network conditions require their own measurements.
