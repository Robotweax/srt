# Live timing, TSBPD, and MPEG-TS/PCR

Robotweax SRT treats time as transport metadata. It does not infer scheduling
semantics from application payload bytes. The same Live timing contract
therefore applies to MPEG transport streams, elementary streams, telemetry,
and arbitrary binary messages.

## Optional receiver phase diagnostics

For local timing investigations, add `--write-phase-events phases.jsonl` to
`interop/run_live_timing_interop.py` (invoked with `./tools/python`). This opts
the timing receiver into `--phase-timing`, adding two `srt_time_now()` reads per
message. Without the option, those reads and JSON fields are absent. The
scorecard records `measurement.phase_timing_enabled`; its existing limits and
end-to-end release/egress measurements remain unchanged.

The separate JSONL file records raw monotonic microsecond boundaries and:

| Field | Interval |
| --- | --- |
| `receive_call_microseconds` | Immediately before `srt_recvmsg2` to the existing release observation |
| `post_receive_microseconds` | Release observation to immediately before the UDP send helper |
| `udp_send_call_microseconds` | UDP send helper entry to the existing egress observation |
| `between_receives_microseconds` | Previous egress observation to the next receive entry; `null` for the first message |

These are elapsed times, including clock-read overhead, blocking, and thread
descheduling, not CPU costs. In particular, the receive interval includes
intentional TSBPD waiting and waiting for data. The UDP interval includes the
helper and `sendto`, not just kernel execution. A long interval alone does not
prove a protocol defect, a lock bottleneck, or scheduler interference.

Observations are buffered and emitted after the receive loop. Run serial
controls without instrumentation alongside diagnostic runs, retain failures,
and correlate an actual outlier with scheduler/profiler evidence before making
causal claims. Extra clock reads and profiler sampling can perturb results.
The phase output is diagnostic evidence, not an alternative acceptance gate.

### Linux-only CI investigation

The separate `Timing diagnostics` workflow runs its timing investigation on
`ubuntu-24.04` when its own workflow, timing runner, trace wrapper, or timing
runner tests change in a pull request. Its manual `investigation` input also
allows selecting `timing` or one of the isolated group investigations described
in [Testing](testing.md#isolated-group-investigations). The registered workflow
supports dispatch against a branch ref containing the selected implementation;
this does not require merging that branch first. Ordinary protocol changes do
not automatically select the timing investigation. It does not replace the
regular CI timing gates or add itself to `Required CI gate`.

The `timing` selection builds only Robotweax peers in a fresh Release directory,
then runs ten serial control/phase pairs and three additional phase runs under
`strace`.
There are no automatic measurement retries. A failed scorecard remains failed;
the remaining planned runs still execute and the workflow returns failure if
any run fails. Each run has a 90-second bound, with its process group terminated
on timeout before another measurement starts. Evidence is uploaded even after
failure and retained for 14 days; archive it before expiration when needed.
This timing job builds no Haivision binaries. The separately selected group
jobs build pinned Haivision reference peers locally on the runner, but upload
only measurement data, logs, and text metadata. No selection uploads Haivision
binaries. Group diagnostic jobs run only by manual selection, not on PR events.

`interop/run_timing_diagnostics.py` implements the same bounded series locally
on POSIX systems. Use `--trace-repetitions 0` when Linux/strace is unavailable.
Its output directory must be empty. Controls, phase runs, and traced runs have
separate directories with commands, scorecards, receiver logs and per-message
events. The optional harness argument `--write-receiver-log FILE` retains the
receiver JSON output, including phase clock anchors when enabled.
The raw receiver log is retained even when transfer validation fails; unlike
the validated event files, its contents must not be assumed complete or valid.
For completed backup-path-outage transfers, `FILE.path-outage.json` additionally
retains the outage coordinator observation and per-path DATA header metadata
(including retransmission flags and ciphertext hashes, never payloads or keys).
This sidecar is copied before the temporary directory is removed even if
subsequent validation fails. Incomplete DATA traces carry an error and remain
test failures. Failures before the transfer completes may have only a partial
receiver log and no sidecar.

The primary-path outage is triggered by a count of original DATA packets.
Retransmissions are retained and checked for destination identity, but do not
advance the trigger count in either injection or validation.

`strace` follows only the receiver and its threads, logging selected wait and
network syscalls with Unix timestamps and elapsed durations. Packet buffers
are not decoded. Phase-enabled receivers bracket a realtime clock observation
with SRT monotonic observations before and after the message loop. Use those
bounds to correlate `strace -ttt` timestamps with phase events; check both anchors
for clock-offset changes and retain the bracketing uncertainty. Neither the
trace nor a long syscall duration distinguishes all on-CPU/off-CPU causes.
Ptrace overhead can cause traced runs to exceed the unchanged timing limits;
such results must not be pooled with controls as a performance estimate.

## Clock domains

Keep these clock domains distinct:

1. **Application source time** is the submission coordinate supplied through
   `SRT_MSGCTRL.srctime` in the monotonic microsecond domain returned by
   `srt_time_now()`.
2. **SRT DATA timestamp** is a 32-bit, connection-relative wire timestamp
   derived from that source time.
3. **Receiver TSBPD time** is the receiver's unwrapped, drift-corrected mapping
   of the wire timestamp plus negotiated receive latency.
4. **MPEG-TS PCR** is media-clock metadata carried in selected 188-byte
   transport-stream packets. It is not an SRT clock.
5. **Wall clock** and optional kernel/NIC timestamp domains are observations
   owned by the application or measurement system, not inputs to ordinary SRT
   release scheduling.

Do not put raw 27 MHz PCR ticks, UTC values, or an unrelated host's monotonic
value into `SRT_MSGCTRL.srctime`. An explicit value must share the local
`srt_time_now()` coordinate. Comparing source and receive times from different
hosts requires an application-measured clock offset.

## Sending source time

For Live/Message sends:

- `srctime == 0` asks Robotweax to capture the current connection time at
  submission;
- `srctime > 0` supplies an explicit absolute microsecond coordinate in the
  local `srt_time_now()` domain; and
- a negative value is invalid.

Robotweax subtracts the immutable connection origin and writes the resulting
low 32 bits into DATA. The wire value is unwrapped at the receiver, so normal
32-bit timestamp rollover does not reset the timeline.

One SRT message has one source time and one release deadline. The timestamp is
not recomputed per DATA fragment.

### Gateway ownership

An SRT gateway application chooses the outgoing source-time policy. It may:

- forward the mapped `srctime` returned by an incoming SRT receive path when
  both paths use a compatible monotonic coordinate; or
- parse PCR or another media clock, map it into local `srt_time_now()` time,
  and generate a new outgoing source timeline.

Robotweax supports both approaches and chooses neither. PCR discontinuity,
program selection, media-clock recovery, and inherited-versus-regenerated
timing are application policy.

For sources without a media clock, explicit or automatically captured SRT
source time remains fully supported.

## TSBPD negotiation and origin

TSBPD is negotiated during HSv5. The effective receive delay is derived from
the local receiver latency and the peer's advertised sender latency. The
receiver establishes one immutable mapping origin from handshake arrival and
the peer handshake timestamp.

For every complete ordered Live message, the receiver:

1. unwraps the 32-bit DATA timestamp around the connection timeline;
2. maps it into the local monotonic domain;
3. applies the negotiated receive latency and bounded drift correction; and
4. withholds readability until the resulting deadline.

The `srctime` returned by `srt_recvmsg2` is this mapped TSBPD play time. It is
not guaranteed to equal the sender's original numeric coordinate plus a fixed
latency, because sender and receiver clocks differ and sustained drift
correction can adjust the mapping.

Applications that need full observability should retain four separate values:

- sender source submission time;
- mapped receive-side TSBPD time;
- actual SRT receive/release time; and
- downstream application or UDP handoff time.

These values describe different stages and must not be collapsed into one
timestamp.

## Receiver scheduling

The receive path enforces these invariants:

- a message is never reported readable before its TSBPD deadline;
- blocking receive and SRT epoll wait for the earliest exact deadline;
- packet arrival, drift correction, close, and loss-state changes wake or
  recompute the wait;
- release scheduling uses a monotonic deadline rather than a fixed polling
  interval; and
- no permanent per-connection TSBPD thread is required.

The short channel-service interval still drives UDP service and protocol
timers. It is not the application release cadence.

After peer SHUTDOWN, complete buffered messages still pass through their TSBPD
gate before the receive path reports its terminal result.

### Gaps and TLPKTDROP

Ordered delivery stops at a missing sequence. With negotiated TLPKTDROP
enabled, when the first later complete message reaches its release deadline,
the receiver may:

- discard the preceding missing or incomplete range;
- remove that range from loss tracking;
- advance cumulative acknowledgement state;
- send the compatible fake ACK; and
- release the due message.

With TLPKTDROP disabled, the gap continues to block ordered delivery until it
is recovered or the connection terminates.

### Drift correction

Clock recovery samples only unique, forward-progressing source packets
observed directly on the wire. Retransmissions, duplicates, late originals,
and FEC reconstructions retain their timestamps for delivery but do not steer
the receive clock with delayed or synthetic arrival times.

A completed estimator window changes the target phase by at most 5 ms, and the
delivery clock approaches the target at no more than 1000 ppm. This bounded
slew prevents a correction from moving queued deadlines backwards or
collapsing correctly spaced messages into an artificial catch-up burst.

## Connection Group time base

A sender Broadcast or Backup group has one logical source timeline, even when
members connect at different times.

Robotweax retains the absolute application `srctime` once per logical message.
All group members use a common timestamp origin on the wire, including during
handshake and after a late join. The receive side shares its TSBPD timebase,
rollover tracking, and slewed drift estimate across members. Both readiness
and the returned delivery timestamp use that same clock; the group retains
the largest negotiated receive delay seen by the clock.

Each successful receive retains the exact deadline used to authorize its
message removal. A concurrent late join or drift update cannot rewrite the
returned `srctime` of that message. Clock changes observed before the removal's
readiness check apply to that check; later changes apply to subsequent checks.

Earlier Robotweax builds used independent member origins and receive clock
estimates. For Robotweax-to-Robotweax groups, update both endpoints to obtain
the synchronized group timeline; mixed-version group timing is not qualified.
Individual connections are unaffected by this group-specific correction.

The following operations do not reset the application timeline:

- a Broadcast member joining late;
- a Backup member becoming active;
- member replacement;
- replay of an unacknowledged logical prefix; and
- 32-bit DATA timestamp rollover.

Backup replay retains the original logical source time and converts it through
the replacement member's origin. When encryption is enabled, the member also
uses its own keys and ciphertext; the group does not reuse another path's
protected payload.

Group status observed at application receive time is not necessarily the path
that originally queued an already buffered redundant message. Timing and
duplicate suppression therefore follow logical message identity rather than a
single status snapshot.

## Message framing

The common 1,316-byte broadcast message consists of seven 188-byte MPEG-TS
packets. It is a useful application profile, not a Robotweax limit.

Live/Message applications may submit:

- one 188-byte transport-stream packet;
- several complete transport-stream packets, such as 376 or 1,316 bytes; or
- arbitrary binary payload up to the negotiated MSS-derived payload limit.

One SRT message receives one transport deadline. A 1,316-byte message does not
receive seven independent 188-byte release points. Applications that require
188-byte release granularity must submit 188-byte messages or pace individual
packets after reception.

File/Stream mode is a continuous arbitrary-byte transport. It disables TSBPD
and does not expose application message boundaries.

## MPEG-TS and PCR ownership

Robotweax does not parse, rewrite, regenerate, or pace from PCR in the
transport core. PCR remains byte-for-byte application payload.

Consequently:

- TSBPD reconstructs the pattern represented by SRT source times, not PCR;
- an already burst-collapsed input remains bursty if the sender supplies no
  better source timeline;
- PCR wrap and discontinuity flags are media-layer concerns;
- SRT loss recovery, FEC, encryption, and group failover must preserve PCR
  bytes but do not interpret them; and
- PCR-to-egress rate and jitter are external quality observations, not inputs
  to SRT scheduling.

A media-aware adapter may derive source time from PCR, but that adapter must
own PID selection, discontinuities, clock mapping, and malformed-input policy.
Such work must not be inserted into the SRT packet hot path.

## Encryption, FEC, and recovery

Timing metadata is independent of payload protection:

- AES-CTR and AES-GCM retain the message's source time through acknowledged
  key rotation and retransmission;
- authentication completes before GCM plaintext enters the receive timeline;
- FEC reconstruction restores the original DATA timestamp before TSBPD
  release;
- recovered, duplicate, or reordered packets cannot create a new source
  coordinate; and
- Backup replay reuses logical source time while encrypting independently for
  the replacement path.

FEC geometry and encryption overhead reduce the allowable payload size but do
not alter the transport-clock model. Binary and MPEG-TS messages use identical
scheduling rules.

## Downstream observation

An application forwarding released messages to UDP or another transport owns
the downstream timestamp and framing policy. A userspace timestamp captured
after successful `sendto` represents userspace send completion; it is not a
NIC wire timestamp.

Where supported, software-kernel or hardware transmit timestamps must be
reported as separate observations with their actual clock domains. They must
not silently replace the portable monotonic userspace baseline or be labelled
as hardware evidence when produced in software.

For single-host timing analysis, a useful event record contains:

- logical message index;
- source submission time;
- mapped TSBPD time;
- actual SRT release time;
- downstream handoff time;
- payload size and optional payload hash; and
- optional PCR value and discontinuity marker.

All compared monotonic microsecond values must share one coordinate. A
multi-host system must measure and apply clock offset before comparing them.

## Operational limits

- TSBPD is a transport scheduler, not a media-clock recovery or PCR-pacing
  engine.
- The library preserves the source pattern it is given; it cannot repair a
  semantically incorrect or burst-collapsed source timeline without
  application help.
- User-space release and egress timing is subject to operating-system
  scheduling. Robotweax does not promise hard real-time or universal jitter
  limits across platforms and workloads.
- Single-host monotonic values cannot be compared directly with a different
  host's monotonic clock.
- Software transmit completion is not physical wire-time evidence.
- Application-specific latency, jitter, burst, PCR-rate, failover, and clock
  quality requirements must be validated on the intended platform and network.
