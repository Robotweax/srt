# ARQ-first FEC and tail loss probe: Stage A record (3 November 2026)

Stage A of the opt-in native integration proposal
(`docs/tail-probe-native-integration-proposal-20261103.md` in the private
adaptive recovery repository, merged as its pull request 88): unit tests at
the pinned seams. This record states what was built and run, the exact
commands, their outputs and the limits. It authorizes nothing beyond itself;
Stage B (deterministic regression against the sealed ledgers) and Stage C
(socket-level contract) need their own authorization.

## Base and branch

- Pin: Robotweax/srt `main` at `1bdd4037cf714ea4e07725d945bb305a182dfbc1`,
  confirmed equal to `origin/main` after `git fetch origin main`; the
  branch was reset to that commit before any change.
- Commits on the branch, in order, each authored Marc Cymontkowski
  <srtpioneer@proton.me> without attribution lines:
  1. `fe892d9` tests: capture the pinned live stream digests for Stage A
     byte identity (the fixture alone, verified passing on the untouched pin);
  2. `de0ad3d` parity policy header, `fec_policy` and `sender_tail_probe`
     options, `SRTO_ROBOTWEAX_FECPOLICY` / `SRTO_ROBOTWEAX_TAILPROBE`;
  3. `7e3667c` runtime decision point, session probe clock, statistics
     counters, connection-setup re-check;
  4. `7971104` Stage A tests;
  5. this record and the option documentation.
- Nothing was pushed; no pull request was opened. The private study
  repository and the main checkout outside this worktree were not modified
  (`git status` clean in both before and after).

## What was integrated (proposal section 1)

| Seam | Where |
| --- | --- |
| `SocketOption::fec_policy` (`FecPolicy::always` default, `arqfirst`) and `SocketOption::sender_tail_probe` (0 default, 1), appended after `experimental_acp1` | `include/robotweax/srt/socket_options.hpp`, `src/socket_options.cpp` |
| `SRTO_ROBOTWEAX_FECPOLICY = 0x01000016`, `SRTO_ROBOTWEAX_TAILPROBE = 0x01000017`, values `ROBOTWEAX_SRT_FECPOLICY_ALWAYS` / `ARQFIRST`; pre-connection; group sockets and members reject the non-default values; follows the `SRTO_DRIFTTRACER` forwarding pattern | `include/srt/srt.h`, `src/compat/socket_options.cpp` |
| Acceptance in `SocketOptions::set`: live, TSBPD, live controller, no rendezvous, enabled non-sensor non-experimental filter with `arq:always` and `rows >= 2`; probe only with `arqfirst`; later changes that would leave the bundle are rejected; re-checked at connection setup against the negotiated filter, matrix encoder, TSBPD, group and rendezvous | `src/socket_options.cpp`, `ConnectionRuntime` constructor in `src/compat/transport_runtime.cpp` |
| `parity::{Mode, Level, Kind, Constants, Inputs, arq_meets_deadline, decide, emit}` in substance from the PoC header, namespace `robotweax::srt::parity`, plus `probe_interval`, `probe_limit` | `include/robotweax/srt/parity_policy.hpp` (new) |
| Decision point: in `poll_locked()` before a ready repair is paced and submitted, `parity::decide(Mode::deadline_only, session_.parity_inputs(now), session_.parity_constants())`; `Level::none` consumes the control (`consume_fec_control()`) and counts `parity_withheld` | `src/compat/transport_runtime.cpp` |
| Inputs `rtt().has_sample()/smoothed/variation`, `Constants::latency_us = peer_receive_delay_milliseconds * 1000`; `detect_us`, `nak_interval_us`, `guard_us` unchanged | `include/robotweax/srt/session.hpp` |
| Probe clock (idle reference, due, entries, fired per idle period), `poll_tail_probe(now)` from the sender tick next to `poll_sender_retransmission_timeout`, idle reset in `note_data_packet_sent(now, retransmitted=false)` as called from `complete_datagram`; the only caller of `request_retransmission_of_last_sent()` | `include/robotweax/srt/session.hpp`, `src/session.cpp`, `src/compat/transport_runtime.cpp` |
| Counters `parity_withheld`, `tail_probes_sent` appended to `StatisticsCounters`; `SRT_TRACEBSTATS` unchanged | `src/compat/statistics.hpp`, `src/compat/statistics.cpp` |
| Wake-up: a due probe bounds the short polling wait like a retirement deadline | `src/compat/transport_runtime.cpp` |

Unchanged on purpose: filter-string grammar, `parse_packet_filter_configuration`,
`negotiate_packet_filter_configuration`, handshake extensions, wire format,
every default, `SRT_TRACEBSTATS`, `detail::SessionTestAccess` (still only a
friend declaration; no production path uses it),
`src/compat/public_socket_options.hpp` (the native `SocketOptions` holds
the state, so no public-options field was needed).

Decisions taken where the proposal left room, stated so they can be reviewed:

- Native rejection value. The native `Error` enumeration has no
  `invalid_parameter`; every rejected option value in this codebase returns
  `Error::invalid_state`, which the C API maps to `SRT_EINVPARAM`
  (`invalid_parameter()`). The new options follow that convention. The tests
  assert `Error::invalid_state` natively; the C-level result is
  `SRT_EINVPARAM`.
- Probe chain reference time. Rule 4 chains later due times from "the
  previous probe's fire time"; the integration uses the evaluation time of
  the due entry (`poll_tail_probe`'s `now`), which equals the due time in
  the fixture and in the tests, and is the actual fire time when the
  runtime polls late.
- Arming test. Rule 1 is evaluated at the due time with the estimator's
  current values, as the fixture does (`arq_meets_deadline` at the due
  time), through `ReliabilitySession::parity_withheld(now)`.
- `tail_probes_sent` counts probes the clock queued; each leaves through
  the ordinary retransmission drain and is also counted as a retransmission.
- Setting order. `arqfirst` requires the filter first; later setters that
  would break the bundle (`tsbpd_mode=0`, `rendezvous=1`, file or control
  type, non-live controller, an incompatible or cleared filter) are rejected
  and mutate nothing. Returning to `always` requires the probe off first.

## Tests added (proposal section 4, Stage A)

New cases only; no existing case was edited.

- `fec_policy_and_tail_probe_default_off_and_change_no_other_option`
- `fec_policy_arqfirst_locks_the_accepted_bundle`
- `fec_policy_arqfirst_rejects_every_unsupported_combination`
- `parity_policy_constants_match_the_sealed_reader`
- `parity_policy_deadline_test_matches_the_sealed_reader_table`
- `parity_policy_emit_and_probe_interval_match_the_sealed_reader`
- `send_buffer_tail_probe_keeps_original_timestamp_and_marks_retransmission`
- `stage_a_defaults_reproduce_the_pinned_live_stream_byte_for_byte`
- `stage_a_tail_probe_clock_follows_the_contract_rules`
- `stage_a_tail_probe_is_inert_without_the_options`

- `tests/test_socket_options.cpp`: defaults `always` / 0; every rejected
  combination (no filter, `arq:onreq` default, `arq:never`, row-only
  `rows:1`, column-only `rows:-2`, partial `fec,cols:8`, sensor profile,
  TSBPD off, rendezvous, file mode with and without the filter, control
  profile, file congestion controller, `rwx-repeat-v1` where compiled) returns
  the rejection value and leaves every other option, the filter string and
  the payload limit unchanged; the accepted bundle is locked afterwards.
- `tests/test_parity_policy.cpp` (new): 62-row table for
  `arq_meets_deadline` and `decide` (modes `deadline_only` and `fixed`) over
  latency 120 000, 60 000 and 200 000 us, `rtt_known` false and true, srtt
  0, 5 000, 20 000, 60 000, 80 000, 100 000 us, including the exact boundary
  `bound == latency_us` (120 000 at srtt 20 000 / rttvar 13 250 and 200 000
  at srtt 60 000 / rttvar 13 250) and its neighbours; 10-row table for
  `probe_interval` with the 10 ms floor (srtt 0, 4 999, 5 000) and the
  120 ms cap (60 000, 60 001, 80 000, 100 000). Expected values computed
  with `python3 -I parity_table.py scripts/tail_probe_budget_comparison.py`
  from the sealed reader's functions, `CONSTANTS = (120000, 2000, 20000,
  5000, 100000, 1000000, 2)`; the reader was read, not edited.
- `tests/test_send_buffer.cpp`: the two existing
  `request_retransmission_of_last_sent` cases stay; the new case checks a
  queued probe keeps sequence, timestamp, message number and payload and
  carries the retransmitted flag.
- `tests/test_session.cpp`: byte-identity fixture (1 024 solo messages of
  1 250 bytes at 1 ms, 40 ms one-way each direction, 120 ms negotiated
  delay, Matrix 8x8 even `arq:always` policy, sources 9, 10, 11 and 600
  lost once). FNV-1a digests over every datagram each session puts on the
  wire and every delivery were captured on the untouched pin and are
  asserted:

  | Record | Digest | Records |
  | --- | --- | --- |
  | sender wire (1 024 originals, 4 retransmissions, 104 controls) | `91c6af16db159c02` | 1 132 |
  | receiver wire (controls) | `b7ca47e27c3fab7e` | 107 |
  | deliveries | `dcc510a0768c4745` | 1 024 |

  With the options off the same digests are reproduced, the policy reads
  `always`, no step would withhold a repair, no due entry and no probe
  occur. With the options on, the probe clock follows rules 1 to 4: not
  armed without an RTT sample or when the deadline test fails (parity
  emitted), due at `max(2*srtt, 10 ms)` capped at 120 ms, at most three
  entries per idle period, the chain continues past a suppressed due time,
  an original resets the period, ACK, NAK, retransmission and probe
  submissions do not, and nothing fires with `packets_in_flight() == 0`. A
  third case shows the clock inert with the options off and with
  `arqfirst` alone.

## Builds and commands

Host: macOS 26.6.2, Apple clang 21.0.0, CMake 4.4.4, Ninja. Builds ran
sequentially; the normal build first, the sanitizer build after its tests
finished. Both builds use the CI sanitizer job's CMake options so the
`rwx-` filters are compiled and exercised.

Normal build:

```bash
cmake -S . -B build-normal -G Ninja -DCMAKE_BUILD_TYPE=Debug -DROBOTWEAX_SRT_BUILD_FILTER_CONTRACTS=ON -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_FILTERS=ON -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF -DENABLE_AEAD_API_PREVIEW=ON -DENABLE_MAXREXMITBW=ON -DROBOTWEAX_SRT_BUILD_EXTENSION_PROTOTYPES=ON -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_CONTROLLERS=ON
cmake --build build-normal --target robotweax_srt_tests robotweax_srt_c_api_test robotweax_srt_compatible_header_test
```

ASan/UBSan build (the CI `sanitizers` job's flags):

```bash
CFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" CXXFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined" cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DROBOTWEAX_SRT_BUILD_FILTER_CONTRACTS=ON -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_FILTERS=ON -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF -DENABLE_AEAD_API_PREVIEW=ON -DENABLE_MAXREXMITBW=ON -DROBOTWEAX_SRT_BUILD_EXTENSION_PROTOTYPES=ON -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_CONTROLLERS=ON
cmake --build build-asan --target robotweax_srt_tests robotweax_srt_c_api_test robotweax_srt_compatible_header_test
```

Both builds completed with warnings as errors (the default) and no warning.

Test commands, run in each build directory with `UBSAN_OPTIONS=halt_on_error=1`
for the sanitizer build:

```bash
./robotweax_srt_tests stage_a_
./robotweax_srt_tests parity_policy
./robotweax_srt_tests fec_policy
./robotweax_srt_tests send_buffer_tail_probe
ctest --test-dir . -R '^(robotweax_srt_tests|robotweax_srt_c_api_test|robotweax_srt_compatible_header_test)$' --output-on-failure
ctest --test-dir . -R '^robotweax_srt_(tsbpd_rtt|stale_drop_coverage|stale_nak)_tests$' --output-on-failure
```

The `robotweax_srt_tests` CTest entry is the general partition (the binary
with CTest's exclusion list); the three further entries hold the cases of
`test_session.cpp`, `test_send_buffer.cpp` and `test_timing.cpp` that the
general partition excludes.

## Results

New cases, identical output in the normal and the ASan/UBSan build:

```text
[PASS] stage_a_defaults_reproduce_the_pinned_live_stream_byte_for_byte
[PASS] stage_a_tail_probe_clock_follows_the_contract_rules
[PASS] stage_a_tail_probe_is_inert_without_the_options
3/3 tests passed
[PASS] parity_policy_constants_match_the_sealed_reader
[PASS] parity_policy_deadline_test_matches_the_sealed_reader_table
[PASS] parity_policy_emit_and_probe_interval_match_the_sealed_reader
3/3 tests passed
[PASS] fec_policy_and_tail_probe_default_off_and_change_no_other_option
[PASS] fec_policy_arqfirst_rejects_every_unsupported_combination
[PASS] fec_policy_arqfirst_locks_the_accepted_bundle
3/3 tests passed
[PASS] send_buffer_tail_probe_skips_unsent_packets_and_empty_buffer
[PASS] send_buffer_tail_probe_skips_expired_tail_across_sequence_wrap
[PASS] send_buffer_tail_probe_keeps_original_timestamp_and_marks_retransmission
3/3 tests passed
```

General partition, normal build (`ctest` output):

```text
1/3 Test  #7: robotweax_srt_tests ....................   Passed   11.56 sec
2/3 Test #87: robotweax_srt_c_api_test ...............   Passed    0.00 sec
3/3 Test #88: robotweax_srt_compatible_header_test ...   Passed    0.00 sec
100% tests passed out of 3
```

The same partition run directly as the binary with CTest's exclusion list
printed `999/999 tests passed` in the normal build and `999/999 tests
passed` in the ASan/UBSan build, with no `[FAIL]`, no `[SKIP]` and no
sanitizer report; `robotweax_srt_c_api_test` and
`robotweax_srt_compatible_header_test` passed in the ASan/UBSan build
(`100% tests passed out of 2`).

Cases of the gated suites inside that partition (counted from the
`[PASS]` lines against the `TEST(` names of each file):

| Suite | Cases in file | Passed, normal | Passed, ASan/UBSan |
| --- | --- | --- | --- |
| `tests/test_session.cpp` | 140 | 136 | 136 |
| `tests/test_send_buffer.cpp` | 45 | 40 | 40 |
| `tests/test_socket_options.cpp` | 22 | 22 | 22 |
| `tests/test_fec.cpp` | 33 | 33 | 33 |
| `tests/test_packet_filter.cpp` | 14 | 14 | 14 |
| `tests/test_live.cpp` | 7 | 7 | 7 |
| `tests/test_timing.cpp` | 20 | 19 | 19 |
| `tests/test_parity_policy.cpp` | 3 | 3 | 3 |

The remaining cases of `test_session.cpp` (4), `test_send_buffer.cpp` (5)
and `test_timing.cpp` (1) belong to the `tsbpd_rtt`, `stale_drop_coverage`
and `stale_nak` CTest entries:

```text
normal build:
1/3 Test  #8: robotweax_srt_tsbpd_rtt_tests .............   Passed    0.55 sec
2/3 Test #20: robotweax_srt_stale_drop_coverage_tests ...   Passed    0.42 sec
3/3 Test #21: robotweax_srt_stale_nak_tests .............   Passed    0.01 sec
100% tests passed out of 3
ASan/UBSan build:
1/3 Test  #8: robotweax_srt_tsbpd_rtt_tests .............   Passed    1.53 sec
2/3 Test #20: robotweax_srt_stale_drop_coverage_tests ...   Passed    0.86 sec
3/3 Test #21: robotweax_srt_stale_nak_tests .............   Passed    0.03 sec
100% tests passed out of 3
```

## Gate verdict

| Gate | Result |
| --- | --- |
| Every new case passes in the normal build | pass (10 new cases) |
| Every new case passes in the ASan/UBSan build | pass |
| `test_session`, `test_send_buffer`, `test_socket_options`, `test_fec`, `test_packet_filter`, `test_live`, `test_timing` pass unchanged | pass (no existing case edited; counts above) |
| Byte identity with the options off | pass (digests equal to the capture on the untouched pin) |
| No edit to any sealed fixture, reader, results directory or frozen source | pass (the private repository was read only; `git status` clean) |
| No default, filter-string, handshake or wire change | pass by construction; the defaults are `always` and 0, and no negotiation path was touched |

Stage A is retained as passed.

## Limits of this stage

- Everything ran in process on one macOS host. No socket, UDP, thread,
  scheduler, CPU, Linux, Docker, Lab or WAN measurement was made.
- The runtime decision point and the probe tick in
  `ConnectionRuntime::poll_locked` are compiled and run by the unchanged
  socket-level suites only with the default policy. No automated test drives
  a `ConnectionRuntime` with `fec_policy = arqfirst`; Stage B's replay
  through the option path is the first such exercise. The parity decision
  itself is covered at the session seam (`parity_withheld`) and against the
  sealed reader's table.
- The byte-identity fixture is a session-level stream (no FEC encoder in
  the loop); the identical digests show that the sender's DATA and control
  sequence and the receiver's deliveries are unchanged with the options off.
- Only a 120 ms negotiated delay with Matrix 8x8 even `arq:always` is
  qualified by the study's evidence; the option documentation says so.
- The ThreadSanitizer, shared-library, GCM-only and interoperability lanes
  of CI were not run here.

## CI follow-up (3 November 2026, after the pull request opened)

The first CI run of the pull request failed on two causes that the macOS
host build had not shown:

- GCC with `-Werror=missing-field-initializers` rejected the designated
  initialization of `parity::Inputs` in
  `ReliabilitySession::parity_inputs` (`include/robotweax/srt/session.hpp`),
  because `last_nak_us` and `last_burst_us` were left to their defaults.
  Both are now initialized explicitly to `std::nullopt`; the values are
  unchanged, and `Mode::deadline_only` does not consult them.
- The C++ format guard (`tools/clang_format.py check-changed`, clang-format
  22.1.8) reported violations in the changed line ranges of eight files.
  `tools/clang_format.py write-changed` reformatted those ranges only; no
  unchanged line was touched and no code changed meaning.

The other failing lanes were the same GCC error, or matrix siblings
cancelled by `fail-fast`. After both fixes the Stage A commands of
"Builds and commands" were rerun on the host in the normal and the
ASan/UBSan build: every `stage_a_`, `parity_policy`, `fec_policy` and
`send_buffer_tail_probe` case passed (`3/3 tests passed` for each group),
the general partition and the three further CTest entries passed
(`100% tests passed`), with no warning and no sanitizer report. The gate
verdict above is unchanged.

## Review follow-up (3 November 2026)

An external read-only review of the pull request at `a003426` reproduced two
behaviours with a standalone two-runtime simulation (injected time, captured
datagrams, 7 sources of 1,250 bytes at 1 ms, originals 15 and 16 dropped,
120 ms negotiated delay, measured peer RTT 20 ms).

1. **Group template accepted the non-default values.** `srt_setsockflag` on
   a broadcast group returned success for `SRTO_ROBOTWEAX_FECPOLICY =
   ARQFIRST` and `SRTO_ROBOTWEAX_TAILPROBE = true`, although proposal
   section 1.1 and the option documentation say group sockets reject them.
   Cause: `GroupRegistry::set_io_option` validates through a temporary
   option-template record and stamps it with the group id only for a fixed
   list of options, which did not include the two new ones, so
   `set_socket_option` saw no group and accepted. Fixed in
   `src/compat/group_registry.cpp` by adding both options to that list; the
   native and member rules are unchanged. New case
   `compat_group_rejects_arqfirst_fec_and_tail_probe` in
   `tests/test_compat_groups.cpp` covers the group handle at the C API:
   the non-default values fail with `SRT_EINVPARAM`, the defaults are
   readable and settable.

2. **First probe after a cold RTT estimator waits the 120 ms cap.** With the
   first RTT sample arriving 30 ms into the connection, the first probe fired
   at 127 ms (idle start 7 ms + cap) instead of 47 ms with a warm estimator,
   and one tail retransmission then missed its playout deadline (6 of 7
   delivered against 7 of 7). This is the sealed rule, not an integration
   defect: the fixture's `ProbeClock::original` fixes the interval from the
   srtt at idle start and `advance` from the srtt at each due time, and
   `ReliabilitySession` does the same. Recomputing the due time on the first
   sample, or evaluating it lazily, would change the rule and make the
   Stage B replay diverge from the sealed ledgers wherever the estimate moves
   during an idle period. Not changed. The cold-start behaviour is now stated
   in `docs/socket-options.md`; a rule change is left to a proposal
   amendment with its own contract.
