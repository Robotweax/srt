# Application samples: contribution, recovery, and device control

These opt-in executables demonstrate sustained public-C-API workflows. They
share a small binary sample protocol, bounded configuration, exact payload
validation, and JSONL observations. They do not use private SRT headers.

Contribution supports real MPEG-TS ingress, recording, and UDP playback output.
Its default mode, plus recovery and device workloads, uses synthetic 1,316-byte
frames with reproducible bodies and exact payload validation.
Live/Sensor completion means the receiver measured a finite stream; it does not
mean every source frame arrived. Inspect `summary.missing` separately from the
process exit status. Control requires every command to execute in order.

## Build

From the repository root:

```sh
cmake -S . -B build-applications -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_BUILD_EXAMPLES=ON \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
cmake --build build-applications --parallel \
  --target robotweax_srt_contribution robotweax_srt_recovery robotweax_srt_device
```

Executables are in `build-applications/examples/applications`. Multi-config
generators add a configuration directory; pass `--config Release` when building.
Commands below use POSIX shell syntax. The C++ samples and Python helpers also
support Windows; use the corresponding executable paths and environment syntax.
Python helpers require Python 3.10+, with no extra packages. On Windows, replace
`./tools/python` with the selected Python interpreter.

All three support `--help`, `--count`, `--interval-ms`, `--timeout-ms`,
`--latency-ms`, `--passphrase-env`, and `--crypto ctr|gcm`. Both endpoints must
use matching counts and transport settings. Default traffic is 600 frames at
10-ms submission intervals. Only numeric IPv4 endpoints are currently supported.
Timeout must exceed twice the latency plus 1,000 ms for the completion exchange.

For encryption, populate an environment variable in both terminals and pass
its **name**, for example `--passphrase-env SRT_SAMPLE_SECRET`. The applications
never print its value. CTR without a passphrase is unencrypted. GCM requires
both a passphrase and a matching library/application build configured with
`-DENABLE_AEAD_API_PREVIEW=ON`; unsupported selection fails explicitly.

## Graphical console and real video

After building, launch the loopback console with Python and an FFmpeg binary
that supports H.264 (`libx264`), AAC, MPEG-TS, and MJPEG:

```sh
./tools/python examples/applications/console.py \
  --binary-dir build-applications/examples/applications \
  --ffmpeg /absolute/path/to/ffmpeg
```

Open the printed URL, including its session fragment. The console binds only
`127.0.0.1:8787`; `--port` selects another port. No Python packages are required.
The launch token authorizes local controls; requests also validate Host and
Origin. Keep this process console on loopback.

The three tabs control video contribution, recovery, and the device simulator.
Video generates encoded H.264/AAC test media, sends MPEG-TS through two SRT
relay paths, and decodes received frames for a live silent preview. On completion,
the received recording becomes a seekable browser MP4 with audio. To use a local
video instead, launch with `--media /absolute/path/to/video.mp4` and choose the
configured-file source. The source is transcoded to a bounded sample clip.

Choose Backup or Broadcast, duration, latency, and loss parameters before Start.
Pause/Resume relay controls drop all packets on a logical path, including control
traffic. They simulate outages; they do not disable a network interface. A broken
SRT member may need a new run after a long outage; resuming a relay is not an
automatic SRT reconnect. Path labels use complete `path_snapshot` membership
reports and distinguish broken sockets from idle members. Rebuild the contribution
binary with the console; older binaries without snapshots show an awaiting-status
label rather than a cached connection claim. Recovery offers ARQ, Row, Column, and Matrix FEC. Device
shows synthetic Live/Sensor traffic and command execution acknowledgements.

Stop terminates the run's child processes. Completed runs retain `received.ts`,
`playback.mp4`, decoder diagnostics, and `result.json` in the displayed output
directory. Stopped or failed runs may retain partial evidence without an MP4.
Use `--output-dir /existing/directory` to choose the parent directory. Ctrl-C
stops the console and its active run. Runs are bounded to 10–120 seconds.

For standalone real-media contribution, start this receiver, then the sender,
then your MPEG-TS source after the sender's `media_ready` event:

```sh
./build-applications/examples/applications/robotweax_srt_contribution receive \
  --endpoint 0.0.0.0:9000 --media-udp 127.0.0.1:9200 \
  --duration 30 --record received.ts

./build-applications/examples/applications/robotweax_srt_contribution send \
  --policy backup --path 127.0.0.1:0,127.0.0.1:9000,100 \
  --path 127.0.0.1:0,127.0.0.1:9000,50 \
  --media-udp 127.0.0.1:9300 --duration 32

ffmpeg -re -i input.mp4 -t 20 -c:v libx264 -c:a aac \
  -f mpegts 'udp://127.0.0.1:9300?pkt_size=1316'
```

A local player can listen on UDP port 9200, or play `received.ts` afterward.
`--record` overwrites the explicitly selected file. Media mode transports raw TS
without sample framing: each UDP datagram must contain 1–7 complete 188-byte TS
packets. Duration bounds the session; a receiver with no media fails. Media
observations report datagrams, bytes, and userspace interarrival gaps. Encrypted
CLI media uses the same passphrase/crypto options described above; GUI runs use
unencrypted loopback transport.

## 1. Multi-path contribution

Start the receiver and wait for its JSON `ready` event:

```sh
./build-applications/examples/applications/robotweax_srt_contribution receive \
  --endpoint 127.0.0.1:9000 --count 600 > contribution-receive.jsonl
```

In another terminal:

```sh
./build-applications/examples/applications/robotweax_srt_contribution send \
  --policy backup --count 600 \
  --path 127.0.0.1:0,127.0.0.1:9000,100 \
  --path 127.0.0.1:0,127.0.0.1:9000,50 \
  --fail-after 150 --restore-after 350 > contribution-send.jsonl
```

Each repeated `--path` is `sourceIPv4:port,destinationIPv4:port,weight`.
The sample accepts two through eight paths. Port zero requests an OS-assigned
source port. Higher weights are preferred in Backup. `--policy broadcast`
mirrors traffic and demonstrates receiver deduplication using the same setup.

`--fail-after` closes one running member immediately before submitting the
specified zero-based source frame. `--restore-after` adds a replacement endpoint
with the same local application token. Events report the requested transition,
member socket IDs, weights and member states; subsequent `path` events establish
whether the replacement connected. Tokens are local metadata, not transmitted
path labels or per-message receive-path attribution.

For two physical networks, replace both source addresses with the sender's
actual interface addresses and both destinations with reachable addresses of
the same receiver, listening on `0.0.0.0:9000`. Confirm OS routes use independent
links. Source binding alone does not establish physical path diversity. The
loopback recipe proves logical failover and replacement only. Disconnecting a
physical link exercises different detection behavior from explicitly closing a
member; record those experiments separately. Replacement is application-owned
and explicitly triggered in this version, not an automatic reconnect policy.

Success observations: `summary.unique == summary.expected`, zero duplicates,
both close/rejoin events, and a new connected member ID after replacement.
`max_receive_gap_ms` measures the largest userspace gap between unique frames;
it is not an isolated failover latency or hardware wire timestamp. Loss is
reported, not hidden. Real-network runs may legitimately have missing frames.

## 2. Recovery comparison

Run the comparison launcher; it starts and cleans up its own loopback peers and
bounded impairment relay. It runs a baseline, ARQ, Row, Column, and Matrix case:

```sh
./tools/python examples/applications/recovery_lab.py \
  --binary-dir build-applications/examples/applications \
  --drop-every 32 --burst 1 --delay-ms 2 --reorder-every 19 \
  --output recovery-comparison.json
```

Each case sends 320 source frames at 5-ms intervals with 200-ms configured SRT
latency. One JSON result per case reports exact application delivery, FEC supply,
parity, retransmissions, receiver drops, interarrival gaps, and actual relay
impairment counters. Row and Column use `arq:never` to isolate FEC; Matrix uses
`arq:onreq`. Increase `--burst` to demonstrate unrecoverable FEC geometry. A
nonzero missing count is a measurement, not automatically a failed experiment.
A failed connection, corrupt payload or failed completion exchange exits nonzero.

For independent endpoints or custom geometry, run three terminals:

```sh
./build-applications/examples/applications/robotweax_srt_recovery receive \
  --endpoint 127.0.0.1:9000 --filter 'fec,cols:4,rows:1,arq:never'
```

```sh
./tools/python examples/applications/impairment_proxy.py \
  --listen 127.0.0.1:9001 --target 127.0.0.1:9000 \
  --duration 30 --drop-every 16 --burst 1 --delay-ms 2 \
  --reorder-every 19 --reorder-ms 30
```

```sh
./build-applications/examples/applications/robotweax_srt_recovery send \
  --endpoint 127.0.0.1:9001 --filter 'fec,cols:4,rows:1,arq:never'
```

The relay pins one client endpoint and one target. By default it impairs only
forward original SRT source DATA; retransmissions, parity, handshake and reverse
traffic pass through. `--impair-direction reverse` applies the same rules in
the opposite direction. It inspects the public DATA header without decrypting
payloads. Drop periods count original DATA, including application completion
markers, not just workload frames. Delayed packets occupy at most 4,096 queue
slots; overflow/backpressure is counted separately. Runtime is capped at one hour.
This isolates recovery behavior; it does not model a whole WAN, impair every
packet class, or replace netem/two-node qualification.

## 3. Device simulator

Start the device, which opens three listeners. Wait for all three `ready` events:

```sh
./build-applications/examples/applications/robotweax_srt_device device \
  --host 127.0.0.1 --port 9100 --count 600
```

Then start its operator:

```sh
./build-applications/examples/applications/robotweax_srt_device operator \
  --host 127.0.0.1 --port 9100 --count 600
```

| Port | Direction | Profile | Demonstration |
| --- | --- | --- | --- |
| Base | Device → operator | Live | Synthetic video-sized frames with deadline-based transport |
| Base + 1 | Device → operator | Sensor | Independent numbered samples, 100-ms TTL, FEC without ARQ |
| Base + 2 | Operator → device | Control | Ordered actuator increments with request IDs and execution acknowledgements |

The Control lane issues `max(1, count / 20)` commands at twenty times the frame
interval (capped at 1,000 ms). Each command increments a simulated actuator once
and is acknowledged **after execution**. The operator waits for that specific
acknowledgement before issuing the next command. This deliberately simple
workflow separates local send acceptance from remote execution.

Sensor observations retain the highest source sequence as the latest reading
and count later delivery of earlier samples as reordering. A recovered older
sample does not move the latest reading backward. Two losses in a four-source
Sensor row can lose samples while Control commands still recover. The automated
`device-impaired` scenario injects that condition through three separate relays.

Request history is bounded and connection-local. Durable command identities,
cross-reconnect deduplication, real actuators, and camera encoding remain
follow-on work. The console controls and observes this synthetic device workflow. The Sensor channel must not be used for command execution ACKs.

## Shared observations and checks

Stdout is JSONL; stderr contains errors and SRT diagnostics. Events include
`ready`, `stats`, `summary`, `sent_summary`, `path`, `path_closed`,
`path_rejoining`, and `executed_ack`. Socket statistics are cumulative,
non-destructive `srt_bistats(..., 0, 1)` snapshots. A group snapshot describes
logical group activity; member snapshots describe individual paths. Do not sum
group and member counters as if they were disjoint traffic. `rtt_ms` may be an
initial estimate; the optional RTT-provenance API is not integrated yet.
Members can change lifecycle state between a group snapshot and their statistics
query. Contribution reports `stats_unavailable` with the socket, error code and
current socket state for that observation; it does not invent zero counters.
Transport failures, payload validation and unexpected statistics errors remain
fatal.

Outside raw media mode, the sample wire protocol has a fixed frame size, marker, 32-bit source sequence,
and deterministic body. Receivers bound their sequence history to the configured
count (maximum one million). An end marker carries that count; the receiver
allows a bounded drain window before sending a completion acknowledgement.
Repeated end markers accommodate finite Live/Sensor tail loss, but no sample
claims reliable loss-free completion of arbitrary Live traffic. The maximum
gap uses only the receiver's local monotonic clock; it is not one-way latency.

With tests enabled:

```sh
ctest --test-dir build-applications -L application --output-on-failure
```

Seven integration scenarios run in CTR builds, with seven additional GCM cases
in extension-enabled builds. Checks require exact baseline payloads, FEC/ARQ
activity under injected loss, connected replacement members, no duplicate
delivery, complete ordered command execution, and deliberate Sensor loss in the
impaired device case. These tests cover loopback, not hardware path diversity.
The comparison launcher separately exercises Column/Matrix, delay and reordering.

Initial local verification on macOS ARM64: all seven normal Release scenarios
and all fourteen Debug CTR/GCM scenarios passed. The wider Debug CTest run
passed 114 of 115 checks; its remaining remote-harness check passed after
selecting native ARM Python instead of the automatically discovered Intel
interpreter. The standalone interoperability harness passed all 794 tests.
The five-case comparison completed with loss, 2-ms delay and periodic 30-ms
reordering; it preserved the measured unrecovered Row-FEC loss in its result.
Windows/Linux builds and independent physical-path runs remain unverified for
these new applications.

The console API tests run without FFmpeg. To add the real-media integration
check, configure with
`-DROBOTWEAX_SRT_SAMPLE_FFMPEG=/absolute/path/to/ffmpeg`. It verifies byte-identical
MPEG-TS delivery and successful decoding for Backup and Broadcast with member
closure and replacement. The extended Debug application suite has 16 checks.
Browser validation also exercised playback and a simulated relay outage: the
received recording remained byte-identical to the source in that local run.

Physical multi-network validation is deferred until a machine with two independent
links is available. Windows/Linux application builds remain unverified. Further
work includes automatic path recovery, transmitted path labels, and RTT provenance
in the shared observer. This sample set does not claim bandwidth aggregation or
add new transport functionality.
