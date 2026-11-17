<!-- SPDX-License-Identifier: MIT -->
# GCC DATA/KM diagnostic reproduction

This is diagnostic evidence, not a successful qualification or a production fix.
Production input is `4d59666`; exact commands/flags/binary/cache/library identities
and raw/published hashes are in [the record](review-gcc-km.json) and provenance.

The standalone [probe](../../../../tools/diagnostics/crypto_receive_order.cpp)
uses public CryptoSession operations, without clocks, sockets or private access.
It emits six JSON rows: CTR/GCM times ordered, lagging-frontier and advanced-frontier
controls. It returns 0 only if all round trips match, 1 for a DATA mismatch and
2 for fixture/provider errors. Current results: four controls match, two actual
round trips fail in every recorded build. No inversion makes failure a pass.

From a configured static-library build with AES-GCM preview enabled, for example:

```sh
c++ -std=c++20 -I include tools/diagnostics/crypto_receive_order.cpp \
  BUILD/librobotweax-srt.a -lcrypto -pthread -o /tmp/crypto-receive-order
/tmp/crypto-receive-order
```

Use that build's compiler/sanitizer/link flags. The macOS OpenSSL library path and
all Linux compiler variants are explicit in the record; do not use an arbitrary
unbound library as current-source evidence. No CMake/default test is added.

## Native diagnostic overlay

Use an isolated detached worktree at the recorded production commit. Apply
[overlay v2](review-gcc-km-overlay-v2.patch) and copy
[the header](review-gcc-km-overlay-v2.hpp) to `src/diagnostic_trace.hpp`.
The [v1 patch](review-gcc-km-overlay-v1.patch) and
[exact v1 header](review-gcc-km-overlay-v1.hpp.txt) preserve the first capture.
Use the exact recorded configuration, build/focus first, then run the recorded
single-case commands serially with `ROBOTWEAX_TEST_KM_TRACE=1` and the original
faults/deadlines/sanitizers. Trace overlays never enter production source.

The ring holds 16,384 events. Each line is:
`[KM_TRACE] index name monotonic_us context_id a b c d e f`.
Index is atomic reservation order; cross-thread timestamps need not be sorted.
Context IDs are bounded opaque aliases, not addresses. Key IDs use exact equality
of bounded synthetic-key copies in scratch memory, never key/digest output;
copies are wiped at dump. `[KM_TRACE_END]` must report equal offered/written
counts below capacity, and `[KM_KEY_IDS] overflow=0` is required for v2 binding.

| Event | a, b, c, d, e, f |
| --- | --- |
| encode | sequence, generation, synthetic DATA identity, stream position, plaintext size, 0 |
| encoder_key (v2) | generation, opaque exact-key ID, 0, 0, 0, 0 |
| rx_replace | selector, highest sequence, highest known, history size, restored index, highest absolute position |
| rx_key_install (v2) | selector, opaque exact-key ID, highest sequence, highest known, 0, 0 |
| rx_select | sequence, selector, history index (0=current), highest sequence, history size, newest retired ceiling position |
| rx_key_select (v2) | sequence, opaque exact-key ID, selector, history index, 0, 0 |
| ctr_clear / gcm_clear | sequence, selector, Error enum, synthetic DATA identity (or UINT64_MAX), ciphertext size, 0 |
| wire_enqueue_data | sequence, selector, retransmitted, modeled drop, queue result, queued count |
| wire_out_data | sequence, selector, retransmitted, queue path (1=DATA), due_us, send Error enum |
| km_out | control subtype, material selector, queue ordinal, due_us, 0, 0 |
| wire_out_control | subtype, 0, 0, queue path, due_us, send Error enum |
| stale_snapshot (v2) | confirmation origin, now, configured lifetime, confirmed flag, confirmations, exhausted |
| confirm_consider (v2) | reply request serial, active request serial, first send, first received, now, round deadline |
| confirm_state (v2) | active, sent, ordered, operation deadline, confirmation lifetime, confirmed flag |
| confirmed (v2) | reply serial, request serial, first send, now, round deadline, 0 |
| monitor_failed (v2) | peer index, next DATA position, fresh, closed, confirmation origin, queries |
| monitor_counts (v2) | confirmed, stale, unmatched, exhausted, attempts, next wake |

`UINT64_MAX` also represents non-1,316-byte test DATA, so bootstrap packets do not
become payload witnesses. No key/salt/passphrase, wrapped KM/ciphertext/tag or
arbitrary application payload is logged. Corruption identities are synthetic.
A send event proves a send attempt/result, not delivery; receive events supply
the separate decryption witness. Counts include repeated copies/repairs and must
not be called unique lost messages. Full raw logs, analysis scripts, all builds
(including the initial failed trace build) and manifests are retained in the
external milestone archive. No debugger or sanitizer suppression is used here.
