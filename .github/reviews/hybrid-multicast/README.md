<!-- SPDX-License-Identifier: MIT -->
# Hybrid multicast independent review

**Draft review only.** The feature is not merge-ready or production-qualified.
The [complete technical review](https://github.com/Robotweax/srt/pull/387#issuecomment-6722547185) found additional build, Windows,
canonical DATA and automation issues. [Current follow-up](review-follow-up.md)
tracks every finding. Follow-up fixes cover build blockers, harness cleanup,
test partitions and listener-close progress.
Other protocol, automation and integration findings remain open.

## Baseline and evidence identity

The branch starts at `v0.3.2`, commit
`e45d0edd346e5c427d85e4c3b1ac605c81a23397`. The annotated tag object's SHA
`235cbdb6c0b0d1950b3e74d623ffc21108bf3ef1` is not the release commit.
Historical native records report checkout
`997cfe2438eab1b0e9bdac2c797a7be7daf31c11`. The current driver records the source
checkout and a binary SHA, but **does not enforce that the binary was built from
that checkout** (BL-6). Historical records remain evidence for the recorded
binary, not a machine-attested current-source qualification. Build fixes at
`e710d4e6f9ab027483e399f222c8cef7837c8d5d` have separate validation below.
GitHub CI is new evidence and must be assessed separately.

## Implemented behavior and reading order

1. **Ingress and source ownership:** multicast DATA provenance and generation
   fences, strict fixed-source negotiation, independent IPv4 multicast transmit
   socket, established unicast control/repair endpoint validation. Read
   [connection groups](../../../docs/connection-groups.md),
   [socket options](../../../docs/socket-options.md), `connection.cpp` and
   `transport_runtime.cpp` in `src/compat/`.
2. **Canonical DATA and bounded sending:** immutable encoding/key-generation
   leases, publication/admission history, group-owned emission and per-member
   retention, persistent unicast/overlap policy, aggregate wire-budget accounting
   and explicit multicast-source recovery. Review `group_packet_encoder.*`,
   `group_runtime_admission.*`, `group_send_coordinator.*`,
   `group_multicast_emitter.*`, `group_unicast_budget.hpp`, and send/session
   buffers. Audit ownership on partial/native error, busy, expiry and close.
3. **Experimental control and transitions:** bilateral selection/proofs, pairwise
   grant/conclusion binding, native authenticated control ownership, correlated
   policy/status, local DATA progress, fallback detector/controller, exact
   overlap proof and multicast return, startup admission and post-join membership
   recovery. Read [the complete hybrid contract](../../../docs/experimental-hybrid-control.md)
   before the `src/compat/hybrid_*` adapters and their tests.
4. **Qualification and failure paths:** deterministic/injected tests,
   native multi-receiver/key-timing/fault/load cases, and the strict soak driver.
   Inspect `tests/test_multicast_dispatch.cpp`, the `test_hybrid_*` and
   `test_group_*` cases, `tests/hybrid_fault_link.hpp`,
   `tests/hybrid_control_vectors.py`, and `tools/qualify_hybrid_soak.py`.

The full branch adds three opt-in Robotweax C options:
`MCAST_INDEPENDENT` (`0x01000018`), `MCAST_SOURCE` (`0x01000019`) and
`MCAST_COORDINATED` (`0x0100001a`), each prefixed `SRTO_ROBOTWEAX_`.
The two mode options are booleans defaulting to false; `MCAST_SOURCE` is a
Listener PRE-bind `sockaddr_in` source address, configured for the independent
mode (port zero selects an ephemeral port at listen).
Existing C exports/layouts, upstream values and `SRTO_E_SIZE` are preserved;
native C++ consumers must rebuild. Review the full feature diff, not only the
latest portability/clock fixes. The hybrid control/provisioning/automation API
remains internal C++ and requires explicit HC **and** ACP1 build opt-ins,
both default OFF. The public coordinated-source options do not expose that
internal automatic fallback API.

## Review questions and merge blockers

- **Failure-safe harness and GCC diagnosis:** why do GCC sanitizer variants skip canonical DATA and
  why does the `-O1` failure fault inside ASan while unwinding? Separate packet
  send/arrival/retransmission/playout timing from fixture cleanup, runtime
  lifetime and compiler/runtime behavior. Preserve the existing comparisons,
  deadlines, fault model and sanitizer instrumentation.
- Check lock order, callback/provider/channel ownership, cancellation,
  stale-generation rejection, completion-after-close and source-failure recovery.
- Check transactional history/credit admission, same-byte retry/repair, crypto
  generation retention and release, nonce/sequence identity, scheduler service
  exhaustion and bounded work. Group queue: 64 frames; public coordinated group:
  at most 64 members including detached members. Aggregate canonical UC DATA
  budget: saturated `4 * MAXBW`, one configured IP-MTU burst. These limits do not
  prove per-member fairness or whole-process memory bounds.
- Check monotonic ages and clock domains, authenticated claims versus observed
  DATA, correlated confirmation freshness, trusted local cursors/host-loss
  declarations, prefix availability and exact overlap-before-return evidence.
- Review the provisional wire allocations (`0x7f15` fixed-source record,
  internal version-7/`0x7f16` hybrid controls), downgrade rejection, replay,
  KDF/context/role/incarnation binding and credential separation. Runtime KM
  remains ordinary unauthenticated passphrase control. Primary coordinated
  source SESSIONAUTH/ACP1 remains rejected. Shared credentials, no forward
  secrecy, provisional allocations and socket-ID limitations remain documented.
- Decide the supported API/rollout and remaining qualification gates explicitly.
  Independent human protocol, concurrency/lifetime and security sign-off is
  outstanding; this draft is not a request to merge or enable defaults.

No new Haivision interoperability claim is made for this Robotweax-only profile.
The existing API boundary is pinned to Haivision SRT v1.5.7 commit
`899348d8318eb9a3c5a5b6ec43c4a1114288773a`; see
[public API compatibility](../../../docs/api-compatibility.md).
Native hybrid auth v7 is not activated as an ordinary socket handshake profile.

## Post-review build fixes

At `e710d4e6f`, BL-1 guards the investigation fixture's GCM socket option with
`ENABLE_AEAD_API_PREVIEW`. BL-2 explicitly converts only after clamping the
recipient limit to its `size_t` configuration bound.

A fresh Release build with repository feature defaults (AEAD, HC, ACP1 and
MAXREXMITBW OFF; tests/tools/benchmarks ON) passes **84/84 CTest gates** on
macOS ARM64, including 1,225 general C++ and 854 Python cases. Enabled Release
and Debug sanitizer configurations each pass 39 budget/coordinator cases and
six investigation cases, without selected skips or sanitizer findings.
See [the new record](records/review-build-fixes.json), [full CTest output](logs/review-repository-default-ctest.log)
and [regression before the guard](failures/review-bl1-default-before.log).

The Win32 narrowing was verified in the old MSVC log. New hosted BCrypt CI
at `18b4595` passes native Win32/x64 builds and selected provider tests plus
shared x64 Debug/Release checks; [job evidence](records/review-harness-windows-ci.json).
ARM64 is cross-build evidence only. No native Windows execution was performed
on this macOS host. BL-3/BL-4/BL-5 and full Windows hybrid qualification remain open.

## Post-review harness fixes

At `64591511d588416f55123a05bd598c3ef510c412`, fixed packet pools move
from automatic storage to one bounded heap allocation per queue. Capacity, packet bytes and allocation-free
enqueue/take are unchanged. Each queue object falls from 99,920 to 88 bytes on
macOS ARM64; the ten inline soak pools previously occupied nearly 1 MiB of
stack. This is a fixture layout change, not a production resource-bound change.

A clean baseline reproduces GCC Og terminate and GCC O1 ASan SEGV with an
invalid duration **before the fixture exists**. A pool-only control with identical
compiler/sanitizer flags restores a normal FAIL/0-of-1 result in both. This
establishes a layout-dependent unwinding failure; it does not establish a
compiler defect or explain the historical canonical DATA drop.

Cleanup is registered before constructor configuration can install a worker.
It clears hooks and joins workers before first socket close, including partial
construction. Reader RAII covers producer/thread-creation failures and propagates
captured reader exceptions after join. Four forced-failure cases audit cleanup.
Send hooks are published only after thread creation succeeds. The harness and
multicast partitions run serial; existing gate/protocol deadlines are unchanged.
Soak/setup wait messages identify their stage and DATA failures print membership
and control state. Original failed logs are preserved.

Repository-default CTest again passes **84/84**, Python **854/854**, and the four
queue cases pass in each selected build. Native results (counts exclude negative
invalid-duration controls and pure queue cases):

| Build | Native passes / partition cases | Result |
| --- | ---: | --- |
| gcc-og | 9/17 | failure retained |
| gcc-o1 | 12/17 | failure retained |
| llvm-o1 | 17/17 | pass |
| gcc-release | 94/94 | pass |
| mac-release | 94/94 | pass |
| mac-sanitized | 94/94 | pass |

A timed-out partition may not start every case; started counts and all failures
are recorded separately. These are focused gates at the new source, not a fresh
full 716-case or whole-platform qualification. [The harness record](records/review-harness.json)
contains exact commands, flags, binary/cache/log hashes, strict four-second
metrics, failures and any separately attempted GCC 30-second profiles. The
qualification driver still lacks enforced binary/source binding (BL-6).

Current GCC failures remain material. Both sanitizer configurations time out the
30-second network partition. Og also loses control freshness in key/load cases.
Both fail the unchanged four-second DATA profiles; O1 CTR shows canonical later
messages after receiver drops, while O1 GCM also reports undecryptable packets.
One Og CTR mismatch is noncanonical with an undecryptable count of zero. These
observations do not establish lateness as the sole cause; packet/repair/playout
and KM activation evidence is still needed. No 30-second GCC run replaces these
failed smoke gates, and O0 has not been revalidated.

## Deterministic listener-close regression and scheduler fix

Source: `4d59666ebaf54df57b514d1069a5172df440af51`. A real `ListenerHandshakeActor` close can
remain queued behind continuously overdue timer work. The new four-case
regression controls dispatch with a barrier and finite eight-turn timer/service
chains. It covers idle/admission-pending actors with and without persistent
service work. Close must be terminal by the second timer callback, while timer
and service work still progress. The pass criterion uses dispatch counts, not
elapsed time; no socket I/O or sleeps are involved. Finite pressure also makes
the failing test drain safely.

On `1c2388d` plus the test-only patch, **0/4** pass, all at the same close-progress
checkpoint. With the scheduler fix, **4/4** pass as part of **78/78** scheduler/
handshake/executor tests in each of eight builds: macOS Default/opted-in Release,
ASan/UBSan and fresh TSan, plus Linux GCC Release/O1/Og and LLVM O1 sanitizers.
These are four unique regressions, not 32 different tests. No selected skips or
sanitizer diagnostics occur in those focused runs.

Ordinary dispatch now alternates due timers and FIFO work. Persistent services
keep their alternating ordinary/service turns and round-robin selection. Timer
and equal-deadline order, FIFO order within each class, reserved capacity and
idle-wake accounting remain covered by the existing suite. The bound assumes
callbacks return; it is not preemption or an absolute close deadline. One fixed
fairness flag per shard replaces no lifecycle barrier and adds no queue/thread.
The architecture text also corrects an old zero-service-slot statement: code
already provisioned 128 slots per process shard; this fix changes no capacity.

Fresh committed-source validation:

| Build | Passed / selected CTest gates | Result |
| --- | ---: | --- |
| linux-gcc-og | 0/1 | regular failure retained; no timeout |
| linux-gcc-o1 | 1/1 | pass |
| linux-llvm | 1/1 | pass |
| mac-default | 88/88 | pass |
| mac-release | 114/114 | pass |
| mac-asan | 22/22 | pass |
| linux-release | 20/20 | pass |

This includes fresh **88/88 Default** and **114/114 opted-in Release** full
macOS suites, each retaining eight expected Linux-only reuseport/adoption skips.
All detailed LastTest files are preserved in the local archive. The former GCC Og key
case returns a normal freshness failure in about 1.6 s; O1/LLVM pass that case.
GCC O1 and Og load/soak each fail both CTR/GCM cases in both gates (0/2 +
0/2), with DATA/freshness errors and no outer timeout or sanitizer report.
One O1 CTR observation is noncanonical; GCM also records undecryptable packets.
These checks remain separate in the record; a normal failure is not a
passing qualification or proof that all prior native hangs are explained.
C ABI/wire/crypto, 200 ms proof/playout, faults, rates and all operation,
confirmation/recovery/process deadlines are unchanged. No sanitizers are disabled.
The new local TSan configuration is explicit in the record.

[Exact before/after inputs, flags, identities, commands and remaining failures](records/review-listener-close.json)
and [the regression-only patch](records/review-listener-close-regression.patch)
are separate from historical profiles. Next: packet/KM/member-state evidence for
remaining GCC DATA/freshness failures, and deterministic extensions if another
close path still stalls. Source/binary attestation and the other numbered review
findings remain open; no merge/release/default enablement or blanket GCC pass.

## CI partition and quota follow-up

Initial partition source: `2f31b966378a81c74b375dffcd325a401d6137bd`. Multicast dispatch has common,
independent and coordinated partitions. HC builds additionally split startup,
conclusions, control, source errors and rejoin. Hybrid model/investigation cases
and the two multi-receiver fault scenarios have distinct process budgets.
Every new partition remains at 30 seconds; the old HC-only 60-second multicast
exception is removed. The full direct selector/qualification driver is unchanged.

The first matrix below is at 2f31b96, including the complete 111-gate enabled
Release run. It exposed one residual outer timeout in the old four-case key
gate on GCC Og. A subsequent CMake/test-guard/documentation-only commit,
`b2b2a547606684ef7660117371614c03fc3e85eb`, gives each key scenario its own
30-second process. All six enabled test binaries remain byte-identical. Final
configuration registers 114 macOS enabled gates (115 in the Linux builds).
Four key gates are validated separately in each enabled build below; there is
no claim of a fresh complete 114/115-gate execution. A single GCC Og key case
still times out after isolation, so its stall is not solved by partitioning.
Python checks every source case has exactly one owner and that optional
retransmission-limit CI still selects cases moved from the general aggregate.

The old native requirement for a positive quota-wait count was host-dependent.
The complete per-submission wire envelope and DATA/member/repair assertions are
retained. Frozen-clock tests force denied attempts, including an exact wait count
of two; a new coarse-clock 180-packet test proves zero waits can obey the envelope.
The existing coordinator test also requires waits with shared original/repair
credit. Downloaded, SHA-verified before-fix artifacts reproduce the assertion on both
Windows BCrypt and macOS OpenSSL at 4dcaf04. Windows also records an independent
environment skip; its full qualification remains open. This corrects assertion
scope; native Windows after-fix confirmation remains CI work.

Opt-in `ROBOTWEAX_TEST_TIMING=1` prints wall time for each completed case,
including cleanup. It does not change protocol time or default runner output.
Diagnostic baseline timings were 33.56 s for the instrumented general aggregate
(including 11.04 s for six investigation cases) and 60.84 s for all 77 multicast
cases. Those profiling binaries have the baseline plus a timing-only runner
change and are identified separately from new-source validation.

| Build | Passed / selected gates | Slowest affected gate | Result |
| --- | ---: | ---: | --- |
| mac-default | 88/88 | 9.42 s | pass |
| mac-release | 111/111 | 20.07 s | pass |
| mac-san | 18/18 | 20.99 s | pass |
| linux-release | 17/17 | 18.42 s | pass |
| linux-llvm | 17/17 | 19.11 s | pass |
| linux-gcc-o1 | 14/17 | 21.12 s | failures preserved |
| linux-gcc-og | 7/17 | 30.05 s | failures preserved |

Final key split (same binaries, changed case ownership only):

| Build | Passed / key gates | Longest key gate | Result |
| --- | ---: | ---: | --- |
| mac-release | 4/4 | 5.88 s | pass |
| mac-san | 4/4 | 6.34 s | pass |
| linux-release | 4/4 | 5.62 s | pass |
| linux-llvm | 4/4 | 5.66 s | pass |
| linux-gcc-o1 | 4/4 | 5.75 s | pass |
| linux-gcc-og | 2/4 | 30.06 s | failures preserved |

The default run's complete console output proves 88/88 gates. A postprocessing
CTest inventory overwrote its auxiliary LastTest output before correcting a
parser for hyphenated gate names. That detail loss is explicit in the record;
per-case counts/times are not reconstructed or claimed for that run. Subsequent
runs save detail before any new inventory. The console and artifact identities
remain available; no test rerun substitutes for the lost detail.

A bounded debugger probe locates the remaining GCC Og stall in cleanup:
`Peers` calls `srt_close`, `ListenerRuntime::close` waits in
`ListenerHandshakeActor::stop`, and the fault-injection workers are already gone.
One scheduler shard is executing a runtime poll; worker threads and the other
shard are idle. A second, deeper probe instead exits normally with a freshness
failure, so it supplies no additional stopped-state evidence. These observations
locate close completion waiting, not a proven scheduler deadlock or sole cause.
The next investigation needs a deterministic close-completion/fairness regression
and queued actor state, followed by packet/repair/playout/KM/member evidence.
GDB probes disable leak detection solely for ptrace; they are diagnostic only.
[Both complete debugger logs](failures/) and their scopes are retained.

GCC ARM64 profiling also found a receive-buffer CPU oracle taking 39.39 s by
itself; its test and receive-buffer implementation are unchanged from v0.3.2.
The general GCC timing probe was stopped explicitly and has no complete suite
result. This is separate from native DATA/KM and freshness failures. This batch
does not claim the entire GCC sanitizer suite fits a 30-second budget or that
all hosted timing/rotation/platform failures are fixed.

[Commands, flags, identities, gate results and diagnostic limits](records/review-partitions.json)
include all failed attempts. Four-second native smoke is not new 30-second
qualification. Original historical records are untouched. BL-3 remains pending
hosted checks and any remaining budget failures; BL-5's source correction has
local deterministic/native validation, with native Windows results pending.

## Historical results and limits

The original packet's misleading "default" label is corrected below. Every
original local build had AEAD preview ON; the 91-gate build disabled hybrid
control/ACP1, not AEAD. Original file names, records and logs are preserved for
provenance; they do not requalify the updated head.

| Configuration | Full Release gates | Selected sanitizer cases | Strict soak |
| --- | ---: | ---: | --- |
| macOS ARM64, hybrid/ACP1 OFF, **AEAD ON** Release | 91 passed | — | not repository-default |
| macOS ARM64, opted-in Release / Clang sanitizer Debug | 99 passed | 716 passed | CTR/GCM, 30 loaded seconds each per build |
| Ubuntu 24.04.4 ARM64 VM, GCC 13 Release | 100 passed | — | CTR/GCM, 30 loaded seconds each |
| Same Linux VM, LLVM 18.1.3 sanitizer Debug `-g -O1` | — | 716 passed | CTR/GCM, 30 loaded seconds each |
| Linux GCC 13 sanitizer Debug `-O0`, `-Og`, `-O1` | — | 626 focused/runtime cases passed per configuration | **failed/unqualified native profiles** |

Full CTest includes 854 Python cases per Release suite. General C++ gate:
macOS 1,231 hybrid-off/AEAD-on / 1,388 hybrid-enabled; Linux 1,388/1,389 with one native IPv6
multicast skip (`ENETUNREACH`, ifindex 1). macOS has eight expected Linux-only
reuseport/adoption skips. The new clock regression runs in the dedicated
runtime gate. Selected sanitizer sets have 626 focused/runtime + 90 native
cases, no selected skips; repeated profiles do not inflate unique counts.

Every qualified long row reports 499 packets/s, 15,000 large 1,316-byte payloads
plus 16 bootstrap/proof messages per receiver, two receivers and 469 key
switches. Global sampled sender-retention peak: 58 packets; final buffers: zero.
The frozen upper fault profile uses DATA 30/50 ms delays, drop every 17th
accepted send, duplicate every 7th accepted send and retry every 43rd attempt, plus selected KM and
auxiliary-control delays. See [the contract](../../../docs/experimental-hybrid-control.md)
and [all metric records](overview.json) for exact semantics.

At midpoint, trusted membership loss must achieve fresh UC fallback <600 ms,
one failed preparation and real rejoin/new epoch <1 s, then a new exact overlap
proof and MC return/read <600 ms. Thirty loaded seconds are split around that
recovery pause. Phase tails have two undropped witnesses; proof disables loss
while retaining delay/duplication/retry. Explicit test grants are duration-sized
(55 seconds for the 30-second run); operation/confirmation/recovery deadlines
remain unchanged.

Source UC fixed-queue capacity retries are permitted and reported; other queue
overflows and UDP I/O errors remain strict zero checks. Sampled retention is
not exhaustive peak or whole-process memory. This is same-host ARM64 IPv4
loopback with synthetic authorized peers, not physical UC/MC separation, WAN,
Windows/x86, native IPv6 multicast, arbitrary key age/loss, isolated-tail
liveness, uninterrupted playback or long-term/large-scale production evidence.
The full GCC Release build retains one existing `-Wstringop-overflow` warning in
the hybrid-admission test helper; the library's warnings-as-errors remain in force.

### Failure evidence

[Original failed run records and logs](failures/) cover GCC `-O0`, `-Og` and
`-O1`. They are not counted as passing native qualification. A diagnostic run
saw peer 0 expect 31 but receive canonical 32 and peer 1 expect 16 but receive
canonical 17, each with one receiver drop, zero undecryptable packets and zero
sender late drops. The `-O1` record also contains an ASan SEGV during error
unwinding. Instrumentation/scheduling overhead is an inference, not a proven
root cause. LLVM passing does not resolve either GCC failure.

## Reproduction

First verify the actual feature-default configuration in a fresh directory:

```sh
cmake -S . -B build-review-default -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-review-default --parallel 4
ctest --test-dir build-review-default --output-on-failure -j1
```

The opt-in profiles below are separate from that default build. The current
soak driver's source/binary binding defect remains open; use a fresh build and
retain build logs and binary hashes. Do not interpret a checkout stamp as an
embedded binary revision.

Use a fresh checkout/build directory and OpenSSL 3, CMake, Ninja, Python 3.10+
with repository test dependencies, GCC 13 for Release or Clang 18 for Linux
sanitizers. Allow native IPv4 loopback multicast. Keep fixed-port multicast
cases serial within a kernel; isolate all strict measurements from builds or
other test CPU load. Do not modify production network settings for this test.

```sh
cmake -S . -B build-review-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ \
  -DROBOTWEAX_SRT_CRYPTO_BACKEND=openssl -DENABLE_AEAD_API_PREVIEW=ON \
  -DENABLE_MAXREXMITBW=OFF \
  -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_ACP1=ON \
  -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_HYBRID_CONTROL=ON \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF -DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF
cmake --build build-review-release --parallel 4
ctest --test-dir build-review-release --output-on-failure -j1
./tools/python tools/qualify_hybrid_soak.py --build build-review-release \
  --config Release --seconds 30 --output NEW_RELEASE_OUTPUT_DIRECTORY
```

```sh
cmake -S . -B build-review-sanitized -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS_DEBUG=-g -O1' \
  -DROBOTWEAX_SRT_CRYPTO_BACKEND=openssl -DENABLE_AEAD_API_PREVIEW=ON \
  -DENABLE_MAXREXMITBW=ON \
  -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_ACP1=ON \
  -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_HYBRID_CONTROL=ON \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF -DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF
cmake --build build-review-sanitized --parallel 4
set -eu
while IFS= read -r selection; do
  ./build-review-sanitized/robotweax_srt_tests --include "$selection"
done < .github/reviews/hybrid-multicast/sanitizer-selectors.txt
./tools/python tools/qualify_hybrid_soak.py --build build-review-sanitized \
  --config Debug --seconds 30 --output NEW_SANITIZER_OUTPUT_DIRECTORY
```

The selectors plus two soak cases reproduce the 716-case targeted sanitizer
set. Keep sanitizer options at normal defaults. For a failing GCC comparison,
use another fresh directory and `g++` with `-g -O0`, `-g -Og` or `-g -O1`;
start with `--seconds 4` and retain the failure before any rerun. CTest smoke
uses four seconds per crypto profile; it is not equivalent to the 30-second
explicit driver. The reported native hybrid profile is Robotweax-to-Robotweax;
reference interoperability for this new profile remains unclaimed.

Existing GitHub workflows still require explicit opt-in coverage for hybrid
control; generic/default and ACP1-only jobs do not qualify HC. The repository's
`Required CI gate` and all selected PR checks must be inspected separately.
No CI result is promised by the archived local successes.

## Published evidence and provenance

- [Overview and complete metrics](overview.json), [selected build settings](build-settings.json).
- [Four clean long-run qualification records](records/) and [their native logs](logs/).
- Three full CTest console logs, [selected sanitizer names](focused-test-names.json)
  and [the sanitizer diagnostic audit](sanitizer-log-audit.json).
- [Original failures](failures/), [recovered failed-build settings](failure-build-settings.json),
  [original/published hashes](provenance.json)
  and [published-file checksums](SHA256SUMS).

The 580-entry full local evidence archive remains outside Git. This curated
packet includes the essential successes, failures, counts, scopes and replay
commands. Absolute paths and failure memory addresses are redacted; metrics,
IDs, outcomes and original artifact hashes are unchanged. Original binary,
cache and log hashes refer to unredacted local artifacts, not the redacted
published logs. Failed-build flags are recovered only from caches whose hashes
match the original records. The canonical-drop diagnostic omitted source/binary
identity; it remains diagnostic-only with that missing provenance stated, not
retroactively manufactured. `provenance.json` maps those hashes; `SHA256SUMS` verifies the
published files. CMake settings are selected values for fresh configuration,
not caches to restore. Source material and this packet are under the
[repository MIT license](../../../LICENSE).
