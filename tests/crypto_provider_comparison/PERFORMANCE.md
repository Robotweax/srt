# Manual Windows provider diagnostics

These measurements do not change the default backend and are not performance
gates on shared runners. Run the optional qualification workflow to obtain
`provider.csv`, `ctr-sweep.csv`, build metadata and binary hashes. Reference
binaries are not uploaded.

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
