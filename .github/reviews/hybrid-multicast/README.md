<!-- SPDX-License-Identifier: MIT -->
# Hybrid multicast independent review

**Draft review only.** The feature is not merge-ready or production-qualified.
The next engineering task is diagnosis of the Linux GCC sanitizer failures.

## Baseline and evidence identity

The branch starts at `v0.3.2`, commit
`e45d0edd346e5c427d85e4c3b1ac605c81a23397`. The annotated tag object's SHA
`235cbdb6c0b0d1950b3e74d623ffc21108bf3ef1` is not the release commit.
Latest code qualification is at
`997cfe2438eab1b0e9bdac2c797a7be7daf31c11`; subsequent documentation/review-packet
commits change documentation/evidence only. GitHub CI on the pushed branch is
new evidence and must be assessed separately from these local results.

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

- **Highest priority:** why do GCC sanitizer variants skip canonical DATA and
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

## Results and limits

| Configuration | Full Release gates | Selected sanitizer cases | Strict soak |
| --- | ---: | ---: | --- |
| macOS ARM64, default Release | 91 passed | — | feature disabled |
| macOS ARM64, opted-in Release / Clang sanitizer Debug | 99 passed | 716 passed | CTR/GCM, 30 loaded seconds each per build |
| Ubuntu 24.04.4 ARM64 VM, GCC 13 Release | 100 passed | — | CTR/GCM, 30 loaded seconds each |
| Same Linux VM, LLVM 18.1.3 sanitizer Debug `-g -O1` | — | 716 passed | CTR/GCM, 30 loaded seconds each |
| Linux GCC 13 sanitizer Debug `-O0`, `-Og`, `-O1` | — | 626 focused/runtime cases passed per configuration | **failed/unqualified native profiles** |

Full CTest includes 854 Python cases per Release suite. General C++ gate:
macOS 1,231 default / 1,388 enabled; Linux 1,388/1,389 with one native IPv6
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
- [Original failures](failures/), [original/published hashes](provenance.json)
  and [published-file checksums](SHA256SUMS).

The 580-entry full local evidence archive remains outside Git. This curated
packet includes the essential successes, failures, counts, scopes and replay
commands. Absolute paths and failure memory addresses are redacted; metrics,
IDs, outcomes and original artifact hashes are unchanged. Original binary,
cache and log hashes refer to unredacted local artifacts, not the redacted
published logs. `provenance.json` maps those hashes; `SHA256SUMS` verifies the
published files. CMake settings are selected values for fresh configuration,
not caches to restore. Source material and this packet are under the
[repository MIT license](../../../LICENSE).
