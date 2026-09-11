# Manual Windows provider diagnostics

These measurements do not change the default backend and are not performance
gates on shared runners. Run the optional qualification workflow to obtain
`provider.csv`, `ctr-sweep.csv`, build metadata and binary hashes. Reference
binaries are not uploaded.

## ECB control and ARM64 comparison

The manual qualification now measures x64 and native ARM64 separately, using
the pinned SDK OpenSSL recipe (including its assembly-enabled check). ARM64
uses `VC-WIN64-CLANGASM-ARM`; this is not a portable-C/no-assembly baseline.
No claim about old ARM64 OpenSSL builds should be inferred from these results.

`--ecb` measures prepared AES-256 ECB in-place, without counter generation,
XOR or per-packet cleanup. The CNG path uses the exact production key wrapper,
included only in the diagnostic executable. OpenSSL uses a prepared EVP ECB
context with padding disabled. A differential check precedes each size case.
Five rounds alternate providers; 100 batches of 256 calls follow warmup.
The percentiles describe batch averages, not individual calls. Sizes include
1328 bytes (1316 rounded to an AES block) and 16000 bytes. ECB timings are a
control, not an additive decomposition of CTR: memory access patterns differ.

Thierry's referenced benchmark at
https://github.com/lelegard/aesbench/blob/d0c5aa30051a1d7e2ed0e63750b11021ac7e2e84/windows/aesbench-bcrypt.cpp
uses 16000-byte buffers for ECB/GCM, process CPU time, at least two seconds of
CPU time and 100000-call inner loops. Its GCM path uses chained calls, not an
independently finalized/authenticated SRT packet on every call. XTS uses
4096-byte messages. It does not benchmark CTR. These differences preclude
directly equating its bulk throughput ratios with our packet-operation times.
No upstream benchmark code is copied into this test.

## Isolated CTR components

`--ctr-components` compares byte-wise XOR with a diagnostic-only 64-bit
`memcpy` XOR candidate, including unaligned and exact in-place buffers. Before
measurement it checks every length from 0 through 1456 and offsets 0 through 7
against the byte implementation. Counter generation and secure clearing have
separate rows. This is not a production change or a security qualification.

Kernels are kept out of line to prevent loop elimination/fusion. They do not
replicate production inlining, cache effects, 1024-byte chunking, or the entire
CTR call. Counter/erase rows process a block-rounded buffer (and counter fill
also clears its local counter); therefore do not sum/subtract these timings to
claim an exact production cost breakdown. Five rounds rotate kernel order.
Use results to select a candidate, then verify it against the unchanged full
provider benchmark, failure-injection tests, counter rollover vectors and
interoperability before accepting a production optimization.

## Complete CTR candidate

`--ctr-candidate` rotates the retained pre-optimization BCrypt baseline,
the production provider (CSV label `bcrypt-candidate`) and OpenSSL within each
size/in-place case over five rounds. Production changes only XOR:
memcpy-based 64-bit words on ARM64 or exact in-place buffers; byte-wise
otherwise. The old baseline copy is compiled only in the benchmark.
Counter handling and secure erasure are unchanged.

Preflight comparisons against OpenSSL cover 128/192/256-bit keys, unaligned
buffers, tails, chunk boundaries and all-ones IV wrap. Timing uses synthetic
fixed IVs and prepared keys, 200 warmups and 100 batches of 256 calls. This
deliberate benchmark-only reuse is not a safe traffic encryption pattern.
Percentiles are batch-average times; compare variants within this run, not
absolute values from other runners. Failure-injection qualification of the
production change and end-to-end throughput remain required before merging.

## Public-API loopback transfer

Manual qualification also builds `tests/crypto_end_to_end` as separate static
Release executables for BCrypt and pinned OpenSSL. Three serial rounds
alternate backend order. Each executable transfers 131072 application blocks of 1316
bytes over an IPv4 loopback caller/listener connection, using File/Stream
mode, first without encryption, then AES-256-CTR and AES-256-GCM. No artificial
pacing or loss is injected. The receiver compares every byte including the
full message sequence. Failures are retained and fail the job; no retry hides
them. These are independent connections, not a steady-state long soak.

`end-to-end.jsonl` records delivered payload Mbps, wall time and Windows
process CPU time (user plus kernel) for sender and receiver together.
`cpu_core_equivalents` is CPU seconds divided by wall seconds, not a percentage
of the whole machine. Measurement begins after connection/accept and includes
thread startup, payload generation/validation and final sender join, but not
socket shutdown or crypto setup. Short transfers and shared-runner scheduling
limit precision. Unencrypted runs provide a transport/harness control.
These results are not NIC throughput, live MPEG-TS timing or a guarantee that
CPU differences translate into higher application throughput. Local macOS
execution validates the harness only, not Windows provider performance.

## CTR size sweep

`provider_benchmark --ctr-sweep` uses AES-256 and five rounds with alternating
provider order. Each size/provider/round has 200 warmup packets followed by
100 batches of 256 packets. Nonces are synthetic and distinct within each
measurement; do not adapt these diagnostic keys for real traffic.

Reported median and p95 are percentiles of **batch-average time per packet**,
not single-packet latency percentiles. Timing includes IV updates, dispatch,
error checking and encryption, but excludes cipher construction and the
OpenSSL differential check performed after each batch. The final packet of
every batch is checked against OpenSSL. `provider.csv` retains the separate
single-call measurements for all three AES key sizes and CTR/GCM.

The size sweep includes 1008/1024/1025/1040 and 2048/2049 bytes. The current
BCrypt CTR implementation constructs counter blocks, invokes CNG AES-ECB in
1024-byte batches, XORs the keystream and securely erases temporary state.
Crossing a boundary requires another CNG call. A timing step is evidence for
batch-related overhead, not a sampled profile or proof of the entire cause.
Counter generation, XOR and cleanup remain alternative contributors.

## Interpretation and follow-up

Qualification run 34523409570 at revision
57dfc1b11750562103940e36dc6b212b14b51446 measured CTR medians around
1.4–1.6 microseconds for BCrypt and 0.2–0.3 microseconds for OpenSSL with
1200/1316-byte payloads. GCM medians were around 0.3–0.4 microseconds for both.
Those single-call results have visible timer quantization and uncontrolled
shared-runner scheduling. They do not establish network throughput or ARM64
performance. Native ARM64 functional qualification is separate.

Before changing production code, inspect the new sweep and, if necessary,
collect a Windows sampled CPU profile to separate CNG cost from local work.
Before choosing a default, compare serial end-to-end encrypted transfers for
both providers with identical payload, key size, latency and pacing. Retain
integrity failures, CPU utilization, elapsed time and delivered throughput;
include an unencrypted control to identify transport/pacing limits. Loopback
results cannot establish physical-NIC throughput. Do not infer an end-to-end
speedup directly from this microbenchmark or relax correctness gates.
