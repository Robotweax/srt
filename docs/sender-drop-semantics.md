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

The analyzer requires each counted sequence to have an earlier successful
original UDP submission on the monotonic clock. Each counter range must cover
exactly its recorded removals (including 31-bit sequence wrap), in the same
protocol poll and after the removals, with matching packet and payload-byte
deltas. Missing, overlapping or unrelated counter ranges fail validation.
Synthetic regression fixtures exercise these rejection paths; they do not
replace the Linux experiment. The historical observations above have not been
rerun as part of the analyzer-validation corrections.

## Reproducing with build provenance

The fixed runner takes five arguments and requires a fresh output directory:

```sh
bash benchmarks/run_sender_drop_kernel_shaping.sh \
  VM_WORK_ROOT DIAGNOSTIC_INSTALL OUTPUT_ROOT NETEM_RUNNER BUILD_MANIFEST.json
```

Use Linux with Bash 4.4 or newer. Before changing host socket limits or running
any network experiment, the runner checks the manifest's SHA-256 values against
both libraries and both Telemetry peers. It also checks the Telemetry checkout
revision and requires a clean working tree. Mismatches stop the run. Trace files
are isolated inside the new output directory, so stale `/tmp` traces cannot be
appended to a new experiment.

The build producer must create and retain a JSON manifest with this structure
(replace placeholders with the actual build identities and 64-digit hashes):

```json
{
  "schemaVersion": 1,
  "robotweax": {
    "version": "actual diagnostic build version",
    "revision": "40-digit commit containing the instrumentation",
    "buildProfile": "actual OS-architecture-config-crypto-trace profile",
    "librarySha256": "SHA256 of the installed Robotweax library",
    "peerSha256": "SHA256 of the Robotweax-linked Telemetry peer"
  },
  "haivision": {
    "version": "actual reference version",
    "revision": "40-digit reference source commit",
    "buildProfile": "actual reference build profile",
    "librarySha256": "SHA256 of the installed reference library",
    "peerSha256": "SHA256 of the reference-linked Telemetry peer"
  },
  "telemetryRevision": "40-digit clean Telemetry source commit"
}
```

Generate hashes with `sha256sum` on the exact installed files used by the
runner. Use a committed diagnostic source revision, not merely the unmodified
base revision. Retain the build logs tying source revisions to those artifacts.
Version, source revision and build profile are builder declarations: matching a
binary hash verifies artifact identity but does not independently reproduce its
source-to-binary mapping. `evidence/build-provenance.json` preserves this
explicit distinction. The runner forwards the declared identities into the
experiment metadata instead of substituting fixed revisions or ARM64 labels.
