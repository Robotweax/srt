# Robotweax SRT 0.2.6 — Performance Optimization

Status: **release candidate; final qualification and publication pending**.

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

A functional transfer at a fixed offered rate is not a maximum-capacity result.
Performance depends on role, occupancy, pacing, encryption, loss, operating
system and workload. Fewer threads do not by themselves guarantee less CPU or
memory. No universal throughput, latency, CPU or RSS improvement is promised.
Shared-listener parallelism, receive syscall batching, callback failure fallback
and reordering-tolerance changes are not part of this release's new guarantees.
Independent cryptographic review remains outstanding.

Before publication, record the exact final candidate commit, required full CI,
reference interoperability and selected platform/integration outcomes. Validate
installed consumers, C exports and release artifacts on supported targets.
Previously qualified source snapshots are supporting evidence, not acceptance
of a different final release commit. Keep original failures and unresolved
reference/environment findings visible when evaluating support claims.

The distribution recipes under packaging/ target the immutable 0.2.6 source
commit `30505346cc6bb935abf68cab806b69e73d428bc1`. Package-manager and Linux
package qualification are pending. The separately published Homebrew tap still
provides 0.2.5 until its recipe and bottle update is reviewed and qualified;
changing this project's version does not publish a distribution package.
