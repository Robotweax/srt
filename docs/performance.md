# Performance and scalability measurement

Performance claims in Robotweax SRT require reproducible evidence. Source-file
size, lock counts, or one unusually fast local transfer are not substitutes for
a controlled comparison. Protocol correctness and timing semantics remain
mandatory while measuring speed.

## Comparative scorecard

`benchmarks/scalability_scorecard.py` runs public-API peer source linked
unchanged against Robotweax SRT and the pinned Haivision library. It supports
two complementary topologies:

- `process-isolated` uses `interop/api_peer.cpp` and one process for each
  connection endpoint. This is the failure-isolated baseline retained for
  compatibility with version-1 reports.
- `many-socket` uses `benchmarks/scalability_peer.cpp`, one application process
  per side, one application thread, and SRT epoll. It measures library thread,
  RSS, CPU, and throughput scaling without per-connection process overhead.
  Its fair workset permits at most 64 consecutive messages per socket and 128
  messages in one application round, independent of connection count, before
  yielding to the selected library's transport workers. At most 64 packets per
  socket may remain in the public send buffer while the application is still
  enqueueing. This keeps the comparison in steady-state transport rather than
  turning a one-shot localhost burst into a measurement of the hosted runner's
  UDP queue. These values are recorded in the JSON methodology.

Both topologies support four profiles:

- Robotweax sender to Robotweax receiver;
- Haivision sender to Haivision receiver;
- Robotweax sender to Haivision receiver;
- Haivision sender to Robotweax receiver.

Every connection receives an exact deterministic payload. Process-isolated
runs validate output SHA-256; the many-socket receiver validates every message,
logical connection ID, sequence number, and payload byte in memory. Both
implementations receive the same Live/Message configuration,
MSS, payload size, flow window, send buffer, unlimited `SRTO_MAXBW`, latency,
and shutdown grace. The integrity-oriented scorecard explicitly disables
`SRTO_TLPKTDROP`: packets that miss a deliberately dense benchmark burst must
remain observable as late delivery rather than being silently removed from a
throughput result. TSBPD remains enabled. The many-socket topology defaults to
120 ms latency and keeps completed peers alive for a separate 500 ms shutdown
grace; process-isolated runs retain their 20 ms and 50 ms defaults. Grace time
is reported as process-exit tail and is excluded from connection-completion
measurements. The default message sizes are 188 and 1,316 bytes. They
cover one MPEG-TS packet and the common seven-packet MPEG-TS aggregation, but
neither value is a Robotweax limit; `--message-sizes` accepts any valid Live
payload size through 1,456 bytes.

The version-2 JSON report records:

- exact helper paths, SHA-256 identities, optional source revisions, host,
  operating system, Python version, CPU model, and logical CPU count;
- useful throughput and message rate;
- p50, p99, p99.9, minimum, and maximum connection-completion time; the
  many-socket peer reports these directly across its sockets;
- setup time, process-exit tail, and total run time;
- complete public SRT packet/retransmission statistics when available;
- child-process user/system CPU and context switches on POSIX;
- sampled peak aggregate RSS and thread count on Linux;
- every warmup and measured run plus median summaries that exclude warmups.

The report is written atomically after every successful run. A later failure
therefore retains the earlier evidence and records `status: failure` plus its
diagnostic.

## Build the comparable helpers

Build Robotweax in Release mode with protocol tools enabled:

```sh
cmake -S . -B build-performance \
  -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_BUILD_TESTS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=ON \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=ON
cmake --build build-performance --parallel
```

Build the same helper against the primary Haivision v1.5.7 reference. The
following Linux recipe requires its checkout at `reference-srt`, verifies the
exact commit and tracked source state, and uses a fresh build directory:

```sh
set -eu
reference_revision="$(git -C reference-srt rev-parse --verify HEAD)"
test "$reference_revision" = "899348d8318eb9a3c5a5b6ec43c4a1114288773a"
git -C reference-srt diff --exit-code HEAD --
test ! -e reference-build-performance
cmake -S reference-srt -B reference-build-performance \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_STATIC=ON -DENABLE_SHARED=OFF \
  -DENABLE_APPS=OFF -DENABLE_TESTING=OFF -DENABLE_ENCRYPTION=ON
cmake --build reference-build-performance --parallel

c++ -std=c++17 -O2 interop/api_peer.cpp \
  -Ireference-srt/srtcore -Ireference-build-performance \
  reference-build-performance/libsrt.a -lcrypto -lpthread -ldl \
  -o reference-api-peer

c++ -std=c++17 -O2 benchmarks/scalability_peer.cpp \
  -Ireference-srt/srtcore -Ireference-build-performance \
  reference-build-performance/libsrt.a -lcrypto -lpthread -ldl \
  -o reference-scalability-peer
```

The include directory, generated build header, static archive, compiler,
OpenSSL, and system libraries must match the reference build. Record their
identity with the resulting JSON artifact. Never substitute an unpinned system
`libsrt` and call the result a versioned comparison.

Keep the reference checkout unchanged between building and measuring. If its
revision or sources change, rebuild the reference library and both helpers in
a new build directory; relabeling an existing binary is not a new measurement.

## Run profiles

A short Robotweax-only functional measurement can run on a developer machine:

```sh
tools/python benchmarks/scalability_scorecard.py \
  --robotweax-peer build-performance/robotweax_srt_interop_peer \
  --profile robotweax-self \
  --connections 1 \
  --message-sizes 188,1316 \
  --bytes-per-connection 1048576 \
  --warmup 0 \
  --iterations 1 \
  --output build-performance/scorecard-smoke.json
```

The corresponding one-process-per-side smoke is:

```sh
tools/python benchmarks/scalability_scorecard.py \
  --topology many-socket \
  --robotweax-peer build-performance/robotweax_srt_scalability_peer \
  --profile robotweax-self \
  --connections 1,8 \
  --message-sizes 188,1316 \
  --bytes-per-connection 1048576 \
  --warmup 0 \
  --iterations 1 \
  --output build-performance/many-socket-smoke.json
```

A publishable comparative run belongs on an otherwise idle, fixed-frequency
Linux benchmark host with thermal state, power policy, CPU affinity, kernel,
compiler, OpenSSL, NIC/loopback choice, and background services recorded. A
representative invocation is:

```sh
set -eu
reference_revision="$(git -C reference-srt rev-parse --verify HEAD)"
test "$reference_revision" = "899348d8318eb9a3c5a5b6ec43c4a1114288773a"
git -C reference-srt diff --exit-code HEAD --
tools/python benchmarks/scalability_scorecard.py \
  --topology many-socket \
  --robotweax-peer build-performance/robotweax_srt_scalability_peer \
  --reference-peer ./reference-scalability-peer \
  --robotweax-revision "$(git rev-parse HEAD)" \
  --reference-revision "$reference_revision" \
  --connections 1,8,32 \
  --message-sizes 188,1316 \
  --bytes-per-connection 16777216 \
  --warmup 1 \
  --iterations 5 \
  --output robotweax-haivision-scalability.json
```

Do not publish shared GitHub-runner measurements as stable performance claims.
They remain useful only as functional smoke or coarse diagnostic evidence.

## Interpretation boundary

The two topologies answer different questions. Process-isolated runs expose
aggregate system scaling with strong failure isolation but include per-process
overhead. Many-socket runs expose how each library behaves when one application
thread owns many sockets; their aggregate process thread count therefore
includes the selected library's runtime workers. Neither topology alone proves
production behavior across hosts, NICs, CPU affinities, power states, or loss.

The connection-completion percentiles are also not one-way application or wire
latency. Existing TSBPD/UDP scorecards remain authoritative for Live release,
MPEG-TS/PCR, jitter, and burst timing.

When extending a measurement, record the transport features, loss profile,
clock and timestamp source, host topology, compiler, crypto provider, and both
library revisions. Normalize throughput to useful application bytes when
encryption or FEC changes wire overhead.

Performance differences guide profiling and optimization. They never justify
weakening packet validation, encryption, reliability, bounded-memory policy,
TSBPD timing, or interoperability behavior.
