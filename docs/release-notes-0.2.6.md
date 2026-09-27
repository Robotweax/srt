# Robotweax SRT 0.2.6 — Performance Optimization

Status: **published 2026-09-26 as [v0.2.6](https://github.com/Robotweax/srt/releases/tag/v0.2.6)**.

Project version: **0.2.6**. Shared-library C ABI line: **0.2**.
Compatible SRT API and `srt_getversion()`: **1.5.7**. This is an implementation
release, not a new SRT wire-protocol version. Default OpenSSL/AES-CTR behavior
and the default-off AES-GCM extension remain unchanged.

## Runtime and storage changes

- Track the next original send packet with a cursor instead of repeatedly
  scanning the unacknowledged prefix; compact retired receive-loss prefixes once.
- Retry transient UDP send backpressure with bounded pending datagrams. Commit
  pacing and send bookkeeping after successful submission, retaining protected
  retransmission bytes and rejecting stale ACKed or expired data retries.
- Bound per-channel receive, send and connection-poll work. Rotate connection
  visits while preserving absolute deadlines across continuations.
- Park quiet channels on a shared socket-readiness watcher while preserving
  protocol deadlines and timer fallback. Coalesce application receive-release
  notifications and avoid immediate continuation after drained UDP input.
- Use targeted readiness notifications and cached epoll state in the normal
  established-socket path; preserve group, edge-trigger and terminal events.
- Separate payload storage from packet metadata. Reserve the configured capacity
  up front, touch payload pages as used, and recycle payload indices independently
  of the sequence ring. Capacity is not reduced; pages are not decommitted after
  a traffic peak, and full occupancy still needs the full reservation.
- Reuse idle connect-callback workers. Retire the callback service before joining
  workers outside the runtime-generation lock, allowing reentrant TLS cleanup.
- Assign channel affinity only when a start actually activates a channel.
  Remove the redundant timer-cancellation notification; new tasks/timers and
  shutdown continue to signal. No CPU gain is claimed for that last cleanup.
- Honor positive Live MAXBW as an absolute rate; input-rate estimation and
  overhead apply to relative mode. Clarify File-bandwidth and group-error
  measurements and enforce benchmark deadlines during application-window waits.

See [performance measurement](performance.md), [payload storage](payload-storage.md),
[UDP backpressure](udp-backpressure.md), [channel fairness](channel-fairness.md),
[idle readiness](idle-readiness.md), [epoll readiness](epoll-readiness.md) and
[loss-list compaction](loss-list-compaction.md) for contracts and test scope.

## Compatibility and upgrading

The public C export inventory is unchanged from 0.2.5; the intended C ABI line
remains 0.2, subject to final artifact checks. Configured packet capacity,
message/stream modes, encryption, recovery and socket-option support are not
intentionally narrowed by these changes. Existing support boundaries remain in
the [compatibility matrix](compatibility.md).

Rebuild consumers of the source-tree C++ interfaces with matching headers and
library. SendBuffer, ReceiveBuffer and ReliabilitySession layouts/signatures
changed; matching C exports do not establish C++ binary compatibility. These
implementation headers are not the installed public C API.

## Qualification limits and release acceptance

The corrected implementation source is `7ecb60ea8b4faca01ed86237b0cc9dc906350f6e`.
[PR #77](https://github.com/Robotweax/srt/pull/77) fixes a rapid listener restart
failure found by the full tag CI: an in-progress native readiness poll could
retain the UDP port after close. Cancellation now retires that poll before the
socket is closed. SRT wire behavior and socket reuse policy remain unchanged.
[Fix CI](https://github.com/Robotweax/srt/actions/runs/36249699089) passed.
Earlier successful qualification of `daac593` does not establish acceptance of
this corrected implementation; renewed package results are linked below.

A functional transfer at a fixed offered rate is not a maximum-capacity result.
Performance depends on role, occupancy, pacing, encryption, loss, operating
system and workload. Fewer threads do not by themselves guarantee less CPU or
memory. No universal throughput, latency, CPU or RSS improvement is promised.
Shared-listener parallelism, receive syscall batching, callback failure fallback
and reordering-tolerance changes are not part of this release's new guarantees.
Independent cryptographic review remains outstanding.

The release was accepted on tag commit `50cba37bc89f423ee7c0a292c3c3eae71b5fc7f9`,
whose product code, tests and build definitions are identical to
`7ecb60ea8b4faca01ed86237b0cc9dc906350f6e`; later commits change package pins,
the Linux package workflow's source download and documentation only.(https://github.com/Robotweax/srt/actions/runs/36251236629)
and the [Windows SDK and signing run](https://github.com/Robotweax/srt/actions/runs/36251236647)
passed on that commit. Published assets and SHA-256 digests:

| Asset | SHA-256 |
| --- | --- |
| `robotweax-srt-0.2.6-windows-sdk-openssl.exe` | `5f3bec9aca5b300cd47f027e6673b93ed5c75e9b153742d9bb8fe5b74beee5bf` |
| `robotweax-srt-0.2.6-windows-sdk-bcrypt.exe` | `c16767db3f56a5f9e3ae31351ee3c65d575cf80e8564cebe697ceb18b263dc3d` |
| `SHA256SUMS` | `5fb22ab965fd47f7d7d2d9b14a590513f0b44b51b19010d0455016d422d0c282` |

Previously qualified source snapshots are supporting evidence, not acceptance
of a different final release commit. Two earlier tag targets failed their tag
CI (an installed-documentation link and the rapid listener restart above); the
tag was moved before publication, and those failures remain recorded. Keep
original failures and unresolved reference/environment findings visible when
evaluating support claims.

The distribution recipes under packaging/ target the immutable source commit
`7ecb60ea8b4faca01ed86237b0cc9dc906350f6e`.
[Package-manager qualification](https://github.com/Robotweax/srt/actions/runs/36250172158),
[Ubuntu/Fedora packages](https://github.com/Robotweax/srt/actions/runs/36250172274)
and the [Homebrew bottle](https://github.com/Robotweax/homebrew-tap/actions/runs/36250101792)
passed for this corrected source. The separately published
[Homebrew tap](https://github.com/Robotweax/homebrew-tap) provides 0.2.6 with an
Apple Silicon macOS 15 bottle. No vcpkg registry, PPA or COPR is published;
changing this project's version does not publish a distribution package.
