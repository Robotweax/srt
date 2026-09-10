# Testing

Robotweax SRT combines deterministic protocol tests, public API tests,
real-socket integration tests, packaging checks, sanitizers, fuzz targets, and
interoperability profiles. External integrators should run the layers relevant
to their build and deployment rather than treating one loopback transfer as
complete qualification.

## Build and run the default suite

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For multi-config generators:

```sh
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Tests are enabled by default through `ROBOTWEAX_SRT_BUILD_TESTS=ON`. Python
3.10 or newer is required when the suite and interoperability harnesses are
built.

## Discover and select tests

List registered tests and labels before selecting a subset:

```sh
ctest --test-dir build -N
ctest --test-dir build --print-labels
```

Run a name-matched subset:

```sh
ctest --test-dir build -R 'package|abi' --output-on-failure
```

Run tests carrying a label:

```sh
ctest --test-dir build -L package --output-on-failure
ctest --test-dir build -L timing --output-on-failure
```

Selection is useful while iterating, but run the complete suite before
publishing a library or changing shared public behavior.

The unfiltered `robotweax_srt_tests` run includes the compatibility cases;
CTest does not repeat that subset in a second invocation. To diagnose only
compatibility cases in a static build, run `./build/robotweax_srt_tests compat_`
(or the executable in the configuration directory of a multi-config build).
This filtered command does not replace the complete suite or the separate
lifecycle and process-exit tests.

## CI build and selection scope

Reference preparation builds only the peers required by the selected profiles,
with `ROBOTWEAX_SRT_BUILD_TESTS=OFF` and development tools explicitly enabled.
Unit and public API tests still run in their dedicated validation jobs. The
standard interop peer remains in the artifact bundle; handshake/group and
scalability peers are added when those profiles are selected. Reference
Haivision binaries are not included in workflow artifacts.

Changes confined to the Python launcher and explicitly listed CI unit tests
run Python validation without selecting protocol builds. Changes to the
documentation checker additionally execute the documentation check. These
exceptions do not suppress checks selected by any other changed file.

Public consumer validation distinguishes three scopes without reducing the
operating-system matrix:

| Changed input | Validation scope |
| --- | --- |
| Public demos and their test harnesses only | Build the three demos; run example tests with the default library and with AES-GCM enabled on Linux, macOS, and Windows. Do not build the unrelated native suite or shared AEAD package. |
| Explicitly listed package templates, export/ABI manifests, or package-consumer fixtures | Static and shared package/ABI checks with AES-GCM off and on across all three platforms, plus FFmpeg integration and Python/documentation checks. No unrelated protocol interop matrix. |
| Production code or native tests | Retain the existing native Release, sanitizer, and domain-specific interoperability selection. Production changes retain AEAD platform/ABI coverage. |
| Top-level CMake, workflows, classifier, or unknown build/package inputs | Conservative full validation. |

`core_tests` selects the native suite, while `examples` and `package` select
public-consumer checks. Mixed commits take the union: a demo change never
narrows checks needed by a simultaneous production or timing-harness change.
The `aead_platform` flag independently selects AEAD package/ABI steps; demos
alone still exercise their CTR and GCM modes without those additional builds.
Consumer-only CTest calls reject empty selections, and the required gate rejects
unexpectedly skipped jobs that the classifier selected. Explicit manual runs
always retain the complete validation matrix.

### Avoiding duplicate scheduled full runs

The weekly run checks for a successful full `CI` run on `main` for the exact
same commit, created within the preceding 24 hours. Only push, manual, or
scheduled runs from this repository and workflow qualify. The successful
`Required CI gate` must contain the successful `Full validation selected (v1)`
step; a green selective run is not enough. API errors, missing evidence,
unfinished/failed runs, or expired evidence retain the complete matrix.

When evidence qualifies, only classification, the DCO no-op, and the required
gate run. The classification summary links to the original full run. This
scheduled no-op cannot renew the 24-hour window: its full-selection marker is
skipped. Weekly revalidation therefore remains active even on unchanged code.
PR and push selection, test counts, and platform frequency are unchanged.
Concurrent unfinished runs are not cancelled or treated as evidence; they may
still overlap. Use `workflow_dispatch` when a fresh full run is needed regardless
of recent evidence. No reference binaries or additional artifacts are stored.
Evidence is read through GitHub's [workflow runs API](https://docs.github.com/en/rest/actions/workflow-runs)
and [latest-attempt jobs API](https://docs.github.com/en/rest/actions/workflow-jobs).

## Python harness tests

On POSIX development hosts, the repository launcher prefers `.venv`, rejects
Python versions older than 3.10, and supports an explicit interpreter override.

Create a local environment if needed:

```sh
./tools/python -m venv .venv
```

Run the harness tests:

```sh
./tools/python -m unittest discover -s interop/tests -p 'test_*.py'
./tools/python -m py_compile interop/*.py interop/tests/*.py
```

On other platforms, invoke a verified Python 3.10-or-newer interpreter with the
same modules and arguments.

## Static and shared package validation

Test the linkage model you intend to distribute. Keep static and shared builds
in separate directories:

```sh
cmake -S . -B build-static \
  -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_BUILD_TESTS=ON
cmake --build build-static --parallel
ctest --test-dir build-static -L package --output-on-failure
```

```sh
cmake -S . -B build-shared \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DROBOTWEAX_SRT_BUILD_TESTS=ON \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
cmake --build build-shared --parallel
ctest --test-dir build-shared -L package --output-on-failure
```

Package-consumer tests verify installed headers, the exported CMake target,
pkg-config metadata where available, the public version, and the selected API
contract.

## FFmpeg integration gate

The FFmpeg gate builds the pinned source revision recorded in the CI workflow
against an isolated shared Robotweax installation. It validates the opt-in
`srt.pc`, FFmpeg's C configure probe, dynamic provider selection, protocol
registration, and unencrypted plus AES-CTR MPEG-TS transfers.

To reproduce it, build and install Robotweax with
`ROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=ON`, then run:

```sh
tests/ffmpeg/configure.sh /path/to/ffmpeg "$PWD/ffmpeg-srt-stage"
make -C /path/to/ffmpeg -j2 ffmpeg
tests/ffmpeg/run_smoke.sh /path/to/ffmpeg/ffmpeg
```

See [FFmpeg integration](ffmpeg-integration.md) for the complete build and
provider-verification procedure.

## AES-GCM validation

The default build tests the AES-CTR profile. Validate the released AES-GCM
extension in its own build tree:

```sh
cmake -S . -B build-gcm \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_AEAD_API_PREVIEW=ON
cmake --build build-gcm --parallel
ctest --test-dir build-gcm --output-on-failure
```

Also build and run at least one installed consumer against that exact package.
This detects a missing propagated definition or mismatched default/GCM headers
before deployment.

### AEAD timing failure evidence

The AES-GCM timing matrix reports each failed field with its scenario, observed
value and expected value or limit. All 14 scenarios still run, and any failed
scenario keeps the matrix unsuccessful. Missing/non-numeric measurements and
non-finite values are rejected; timing thresholds are not relaxed.

To retain failure diagnostics locally, after building the GCM timing peers:

```sh
./tools/python interop/run_aead_timing_interop.py \
  --timing-peer build-gcm/robotweax_srt_timing_peer \
  --sender-peer build-gcm/robotweax_srt_interop_peer \
  --group-sender-peer build-gcm/robotweax_srt_group_timing_sender \
  --failure-artifacts interop-artifacts/aead-timing
```

Each failing invocation creates a distinct `run-*` subdirectory containing a
scenario diagnostic `.log` and, when available, an allowlisted scorecard `.json`.
The scorecard preserves gate inputs and selected timing summaries, not arbitrary
measurement extensions. Unknown text values are redacted. Raw stdout/stderr,
commands, environment variables, payload captures, and binaries are deliberately
not retained or printed by the matrix's failure reporter. Its log contains field
diagnostics and child exit/timeout status, not raw peer output. If no scorecard
was produced, the log states that limitation; it cannot reconstruct absent
measurements. Failure to write artifacts is also reported without hiding the
original failure. Local artifacts remain until explicitly removed.

The CI `aead-timing` shard uploads only these JSON/log files on failure as
`aead-timing-failure-<run-id>-<attempt>`, with three-day retention. Successful
scenarios create no retained artifacts. This does not change test selection,
acceptance thresholds, or the required CI gate.

## Sanitizers and fuzzing

Sanitizer support depends on compiler and platform. Configure sanitizer flags
in a clean tree, build the test suite, and run the same focused scenarios that
exercise the changed code. Do not combine sanitizer timing results with
Release performance expectations.

UBSan diagnostics must fail the run, not merely print a warning. The CI
Address/undefined job compiles C and C++ with
`-fsanitize=address,undefined -fno-sanitize-recover=undefined` and runs with
`UBSAN_OPTIONS=halt_on_error=1`. Fuzz smoke also sets the fatal UBSan runtime
policy. Recovery is enabled for many checks by default; see the
[UBSan documentation](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html).

To verify a sanitizer build's enforcement before running its tests:

```sh
cmake --build build --target robotweax_srt_ubsan_canary
UBSAN_OPTIONS=halt_on_error=1 ./tools/python \
  tests/sanitizers/verify_ubsan.py build/robotweax_srt_ubsan_canary
UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build --output-on-failure
```

Configure that tree with the sanitizer compile/link flags first. The canary
inherits its compiler and global flags from the same CMake build as the tests;
it does not enable instrumentation itself. Its healthy invocation must succeed,
while an intentional signed overflow must produce the expected diagnostic and
a nonzero exit. An uninstrumented or recoverable build fails this verification,
as does a loader error or unrelated crash. The probe is excluded from the
default build, CTest registration, and installation. Never use sanitizer
instrumentation as a production security boundary.

Fuzz targets are opt-in:

```sh
cmake -S . -B build-fuzz \
  -DROBOTWEAX_SRT_BUILD_FUZZERS=ON \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
cmake --build build-fuzz --parallel
```

They require a compatible libFuzzer toolchain. Retain and minimize any failing
input so the failure can become a deterministic regression test.

## Interoperability qualification

Robotweax-to-Robotweax loopback proves self-consistency, not compatibility
with every third-party SRT build. For each supported deployment profile, test:

- both implementation directions when peers differ;
- Caller/Listener or Rendezvous as actually deployed;
- IPv4 and/or IPv6;
- Message or Stream mode;
- exact encryption mode, key length, and rotation policy;
- MSS and payload sizes at their operational boundaries;
- packet filtering and group policy, if enabled;
- loss, delay, reordering, duplication, shutdown, and reconnect behavior.

Pin the peer release and configuration in your test records. An unspecified
"SRT-compatible" peer is not a reproducible qualification target.

### Isolated group investigations

The `Timing diagnostics` workflow has a manual `group-receive-contract`
selection. It builds clean Release group peers on Ubuntu 24.04 and runs only
Robotweax Caller to Haivision Listener with the Broadcast receive contract,
60 times serially. Haivision 1.5.7 is pinned to commit
`899348d8318eb9a3c5a5b6ec43c4a1114288773a`. Alternatively, select
`group-backup-baseline` to repeat only Robotweax Caller to Haivision Listener
with the Backup baseline, including its existing payload-ready and completion
barriers. Each selection runs only its own scenario, not both. These jobs are
not selected by PR events.

Each failed case remains a failure even if later cases pass. A 40-second
per-case safety bound and a ten-minute series budget prevent an unbounded
investigation; an incomplete series fails. Existing contract assertions and
peer timeouts are unchanged. The artifact retains every case's peer output,
summary, binary hashes, and build metadata for 14 days, including failed
measurements, but contains no reference binaries.

On a POSIX host with matching prebuilt peers, a shorter runner check is:

```sh
./tools/python interop/run_group_diagnostics.py \
  --robotweax-peer /path/to/robotweax_srt_group_peer \
  --reference-peer /path/to/haivision-1.5.7-group-peer \
  --output-directory /path/to/new-evidence-directory --repetitions 2 \
  --scenario group-backup-baseline
```

Use an empty output directory. A successful diagnostic series is evidence for
that host and revision, not proof that an intermittent defect is fixed.

The separate manual selection `group-matrix-context` compares one isolated
Backup baseline, the complete 15-case CI group baseline matrix in its original
order, another isolated Backup baseline, a matrix with opt-in peer phase
timestamps, and a separately identified matrix with those timestamps under
`strace`. Each matrix still stops at its first failure; later scheduled
investigations run without erasing that failure. Isolated cases are bounded at
40 seconds and each matrix at 120 seconds. No peer timeout or assertion changes.

It retains case start/end times, harness PID, endpoint commands, host load and
raw peer logs for the 14 admission/transfer/receive-contract/late-join cases.
The final PEERERROR-isolation case retains its result and any exception text,
plus the matrix stdout/stderr; it does not yet retain separate raw peer files.
The trace follows only the matrix harness and descendants, including process
lifecycle, network calls and waits, without decoding packet/option buffers.
Tracing changes scheduling; do not treat a traced result as an uninstrumented
control or assume a long wait proves a protocol defect. This is group-matrix
context, not a reproduction of every earlier step in the full handshake job.

`interop/run_group_matrix_diagnostics.py` runs this schedule with the same peer
and output-directory arguments. On macOS use `--without-strace` explicitly;
that runs the four investigations without strace and is not a Linux result.
The control sections explicitly disable `ROBOTWEAX_SRT_GROUP_PHASE_TRACE`;
only `matrix-phase` and `matrix-strace` enable it. Phase records contain
monotonic and wall-clock microseconds, operation, begin/end, message index and
API return value, never payloads, hashes or keys. They bracket acceptance,
member readiness, payload receives and hash-reply I/O. Even these small writes
can affect scheduling; `matrix-phase` is not an uninstrumented control. Missing
or unpaired phase evidence cannot qualify an otherwise successful phase run.

## Timing and performance tests

The manual Linux selection `group-strace-stack` compares the original control
matrix, the existing strace matrix, and a separate strace matrix with native
stack unwinding (`-k`, at most 24 frames per syscall). These are three bounded
120-second matrices, not retries; a failure remains a failure even if a later
section passes. Both traced sections use the same syscall selection and phase
markers. The runner retains Release optimization with debug symbols locally;
no reference binaries or cores are uploaded. Logs have a 128 MiB per-file limit.
Stack frames are function names/addresses, not argument values or local-variable
dumps. Missing stack frames fail the stack section. Frames being present does
not prove coverage of the intermittent stall: correlate the exact process,
phase interval and wait call before drawing conclusions. Stack unwinding adds
overhead and can change the symptom. No GDB attachment is used in this mode.

The Linux-only manual `group-receive-stack` investigation runs one unchanged
control matrix and one independent phase-observed matrix, each bounded at 120
seconds. It never attaches a debugger to a strace-controlled process. Both peers
retain Release optimization and add debug symbols on the runner only. If a
reference payload receive remains open for 750 ms, it attempts at most one GDB
snapshot of the verified child process in the entire matrix. The debugger is
limited to two seconds plus one second for forced termination. Stack frames
omit arguments and locals; only selected scalar group receive-state fields are
printed when available. No core dump or reference binary is uploaded.

Attachment pauses the peer and can itself cause a timeout. Treat this matrix as
diagnostic, not timing qualification. A passing matrix with `no-stall-observed`
has no stack coverage and does not close an intermittent finding. Optimized-out
fields are reported as unavailable, not inferred. This mode requires the hosted
runner's passwordless sudo for the narrowly targeted debugger attachment; it
does not alter system-wide ptrace settings or support a silent macOS fallback.

Run timing and throughput tests in Release mode on representative hardware.
Record CPU topology, operating system, compiler, OpenSSL build, NIC, socket
buffers, MSS, latency, payload distribution, and impairment profile.

Single-host loopback results are useful regressions but do not replace
two-node testing, real NIC queues, clock differences, or the production
network. Define pass/fail limits before running a qualification campaign.

## Failure triage

When a test fails:

1. rerun only the named test with `ctest -R` and verbose output;
2. confirm the build directory matches its compiler, linkage, and crypto mode;
3. preserve the first SRT error and peer rejection reason;
4. record the exact peer version and configuration;
5. determine whether the failure is deterministic, timing-sensitive, or
   dependent on external network state;
6. rerun the complete relevant suite after the fix.

Use a clean build directory before attributing a result to source changes.
See [Building](building.md) for cache hygiene and [Known limitations](limitations.md)
for combinations that are intentionally unsupported.

### Pinned reference group receive

The Haivision 1.5.7 group helper is compiled with
`ROBOTWEAX_SRT_REFERENCE_GROUP_RECEIVE=1`. This selects bounded nonblocking
receive only for the helper's payload-range verification. It retries only
`SRT_EASYNCRCV`, retains the five-second per-message deadline and all payload,
hash, sequence and member-metadata checks, then restores the receive mode.
Robotweax's helper and the separate blocking receive-contract probes are
unchanged. This is not a transport-library workaround or a relaxed gate.

The reason is a reproduced Linux reference-side stall: payload was ACKed, but
the reference application remained in `CUDTGroup::recv_WaitForReadReady` /
`CEPoll::swait`; its send queue was empty. The final hash reply appeared only
after the caller timed out and started closing its members. Packet capture
and two post-stall thread snapshots established this distinction from a
Robotweax reply-receive failure. See diagnostic runs
[34282020452](https://github.com/Robotweax/srt/actions/runs/34282020452) and
[34282329261](https://github.com/Robotweax/srt/actions/runs/34282329261).
The precise internal reference readiness race remains unproven; a passing
retry or added logging alone must not be treated as a fix.

With the reference-only nonblocking path, the subsequent
[60-case Linux run](https://github.com/Robotweax/srt/actions/runs/34282747581)
passed without phase logging. This validates the harness mitigation, not a
repair of Haivision's internal blocking implementation.

### Live flight-tail recovery

`robotweax_srt_live_tail_recovery` drops exactly the last original DATA datagram
of a six-second, source-paced Live transfer through the existing deterministic
UDP fault relay. The two-second TSBPD budget is shorter than the source run, so
application reads are already freeing receive-buffer space when the flight tail
is lost. This matters: repeated ACKs can advertise window updates without
advancing the cumulative acknowledgement. They must not restart the sender RTO.

The test requires the tail retransmission, its cumulative ACK, complete payload
integrity and successful peer exits. It leaves no temporary artifacts by default:

```sh
ctest --test-dir build -R '^robotweax_srt_live_tail_recovery$' --output-on-failure
```

To retain the peer logs and fault metadata, or compare a different sender build:

```sh
python3 interop/run_live_tail_interop.py \
  --sender-peer build/robotweax_srt_interop_peer \
  --receiver-peer build/robotweax_srt_interop_peer \
  --artifacts /path/to/new-tail-results
```

This regression preserves the retransmission timeout formula and its backoff.
Only an ACK that advances the send-buffer sequence restarts the timer; valid
non-progress ACKs still participate in receive-window and RTT processing.
