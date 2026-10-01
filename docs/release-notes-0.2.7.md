# Robotweax SRT 0.2.7 — Recovery and Runtime Hardening

Status: **in preparation, not published**. No release tag or 0.2.7 installer
is qualified yet. The latest published release remains 0.2.6.

Project version: **0.2.7**. Shared-library C ABI line: **0.2**.
Compatible SRT API and `srt_getversion()`: **1.5.7**. This is an implementation
maintenance release, not a new SRT wire-protocol version. OpenSSL/AES-CTR remains
the default; the explicit AES-GCM extension remains default-off. BCrypt remains
an experimental Windows backend.

## Recovery and timing

- Correct ACK cadence and RTT sampling, File retransmission capability gating,
  timestamp-origin handling and receive deadlines across idle periods and wrap.
- Correct loss-list invariants, periodic NAK TTL handling and loss-range
  reactivation after control coalescing and transient UDP backpressure.
- Validate FEC geometry and bounded receive windows, preserve recovery across
  sequence gaps and out-of-order loss, and avoid reporting settled FEC losses.
- Validate directional TLPKTDROP behavior and encrypted drop-control sequence
  plausibility. Preserve already buffered stream data when peer drops arrive.
- Retain conservative loss-report and reordering policies where measurements
  do not establish a general improvement.

## Encryption and admission

- Harden optional-encryption state and negotiated cipher reporting, key-length
  admission, rotation sequence budgets and transition/blackout recovery.
- Preserve receive-key identity across refresh and bounded retired history,
  including the later correction that protects a newer generation after it
  has processed DATA.
- Bound KM work and retry/preannounce timing. Long peer RTT requires a suitable
  peer-idle configuration; retry mitigation does not authenticate ACK values.
- Use random socket identities and strengthen handshake window and rejection
  validation. These changes do not authenticate all runtime control packets.

## Groups and runtime

- Correct Broadcast/Backup member option templates, late joins, send
  backpressure, directional admission, state snapshots and logical readiness.
- Preserve complete locally received unread messages when an old member is
  explicitly closed. Receive-only retention is bounded to 8,192 packets,
  11,927,552 payload bytes, 16 batches and 120 seconds of unread age.
  Capacity, allocation and expiry failures become explicit connection errors;
  missing or incomplete messages are not recovered by this retention queue.
- Reject FILE and disabled TSBPD group configurations transactionally.
  Member-specific `GROUPMINSTABLETIMEO` remains unsupported; the supported
  group-wide value and other option limits are documented.
- Correct edge-trigger send rearming, readable-deadline events, readiness-arm
  recovery, close/backpressure lock ordering and shard drain completion.
- Harden callback cleanup/reentry and inherited process teardown. Improve
  readiness invalidation, bound closed-handle history and simplify group
  member lookup/status storage without promising universal CPU or throughput
  gains.

See [encryption](encryption.md), [connection groups](connection-groups.md),
[epoll readiness](epoll-readiness.md), [UDP backpressure](udp-backpressure.md),
[File mode](file-mode.md), [pacer qualification](cr12-pacer-qualification.md),
[host qualification](k43-host-qualification.md) and
[group throughput](group-throughput.md) for detailed contracts and evidence.

## Compatibility and upgrading

The installed public C export inventory is unchanged from 0.2.6; the intended
C ABI line remains 0.2. The release candidate still requires installed-consumer
and export/ABI qualification in its static/shared and platform configurations.
Existing support boundaries remain in the [compatibility matrix](compatibility.md).

Rebuild direct source-tree C++ consumers against matching headers and library.
Implementation layouts and signatures changed substantially after 0.2.6;
unchanged C exports do not establish C++ binary compatibility. Source-tree
implementation headers are outside the installed public API contract.

## Qualification and remaining limits

The starting main commit is `acb515d861725db65425d788990d61d46bad85f2`.
Its [post-merge CI](https://github.com/Robotweax/srt/actions/runs/36930373524)
completed successfully. This is baseline evidence, not acceptance of a future
0.2.7 tag, package source or signed installer. The final candidate must pass
the [release process](release-process-0.2.7.md), including the explicit complete
ecosystem suite; skipped integrations are not new qualification results.

The latest group retention correction passed 893 native tests, 72 final
sanitizer group tests and 24 bidirectional UDP prefix cases covering Clear,
CTR, GCM, Broadcast, Backup and explicit close/keep. These are functional
checks, not maximum-capacity or real-WAN guarantees.

- [Issue #112](https://github.com/Robotweax/srt/issues/112) remains open.
  Runtime control authentication and complete long-horizon KMREQ replay
  protection are not implemented. Bounded defenses do not cover histories
  after eviction. DATA AES-GCM does not authenticate control packets; CTR does
  not provide payload authenticity. Independent cryptographic review remains
  outstanding. No ACP1 wire/API profile ships in this release.
- Native Linux/Windows performance and real-network-path campaigns remain
  outstanding because dedicated target hosts are unavailable. Prepared lab
  packages and macOS runs do not replace these measurements.
- The earlier Windows lifecycle timeout root cause remains unresolved.
  Current successful CI does not explain that earlier timeout; its original
  five-second diagnostic deadline remains unchanged.
- Additional sustained-load, failover and isolated CPU-cost measurements remain
  outstanding. No general retry-policy superiority or capacity claim is made.
- Package-manager coexistence/removal checks are not an upgrade/rollback
  guarantee. No new PPA, COPR, vcpkg registry or application binary distribution
  is promised.

## Distribution status

No 0.2.7 download assets, source archive hash, signed SDK checksum or Homebrew
bottle is accepted yet. Package recipes still select 0.2.6 until the immutable
0.2.7 product source is committed, archived, verified and repinned separately.
Record exact source and tag commits, CI run links and post-signing checksums
before changing this section to published status.
