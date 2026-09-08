# Changelog

All notable project changes are recorded in this file. Robotweax SRT uses
semantic project versions independently from the compatible Haivision SRT API
version returned by `srt_getversion()`.

## Unreleased

- Install the standalone `<srt/access_control.h>` compatibility header with
  all 21 `SRT_REJX_*` application rejection constants, including
  `SRT_REJX_OVERLOAD`. Add installed C/C++ consumer coverage to prevent
  accidental fallback to another SRT provider's header (issue #4).

- Fix cleanup from an application singleton destructor when sockets, groups,
  or epoll resources are first created after startup returns. Initialize
  dormant cleanup dependencies before the application owner completes
  construction; add process-exit coverage, including static SRT embedded in
  a shared library (issue #5).

- Add the opt-in `robotweax_srt_udp_bridge` demo for continuous raw MPEG-TS
  UDP → SRT → UDP forwarding using the public API. Supports IPv4 unicast/multicast,
  Caller/Listener and Rendezvous in either media direction, configurable
  latency, optional AES-CTR, live statistics and opt-in reconnect;
  includes datagram-integrity/rejection tests and a player/source quickstart.

## 0.2.3 — 2026-09-07

Compatibility-preserving maintenance and security hardening. The public C ABI
remains on line 0.2, the compatible API remains SRT 1.5.7, and the AES-GCM
extension remains explicit and default-off. Independent cryptographic review
and the documented group/timing qualification limits remain outstanding.

### Fixed

- Confirm independent initial transmit key/salt material before encrypted DATA
  transmission in each direction; retain the existing runtime key exchange,
  cipher suites and wire layout. Key-control errors cannot release provisional
  plaintext after encrypted negotiation. This can add startup latency.
- Reject native key refresh intervals above `2^31 - 1` before changing state.
- Reclaim closed public socket records and erase their configured secrets.
- Ignore delayed INDUCTION replies after the caller has advanced its handshake
  exchange, rather than allowing stale replies to disrupt progress.
- Honor explicit AES-CTR/AES-GCM selections in all public transport demos.

- Synchronize the timestamp origin and receive TSBPD clock of connection-group
  members, including late joins and replay across timestamp rollover. Member
  switches no longer substitute an independent handshake-derived delivery
  timebase. The shared clock governs release readiness as well as `srctime`;
  timing validation limits are unchanged. Connection-attempt timeout budgets
  remain independent of the age of the group.
- Preserve the deadline snapshot that authorizes each received message, so a
  concurrent group late join cannot change its returned `srctime` after the
  message has already passed the timing gate.

### Build, documentation and validation

- Include FFmpeg in the required CI gate and enforce fatal UBSan findings with
  an instrumentation canary.
- Retain allowlisted AEAD timing failure diagnostics with short-lived,
  failure-only CI artifacts; do not upload raw payloads or reference binaries.
- Include MIT and third-party notices in Robotweax CI tool archives.
- Correct static C pkg-config linking guidance, control-packet documentation,
  FEC statistics accounting and active reference provenance.
- Document a private security-reporting email fallback for new reporters.

See [0.2.3 release notes](docs/release-notes-0.2.3.md) for upgrade guidance,
qualification limits and release acceptance.

## 0.2.2 — 2026-08-31

Robotweax SRT 0.2.2 is a compatibility-preserving patch release that advances
the default public SRT profile to 1.5.7, corrects no-argument control-packet
encoding, and rejects malformed control and key-management input before it can
affect connection state or peer liveness. The shared-library ABI remains 0.2.

### Changed

- Advanced the default public SRT compatibility profile and
  `srt_getversion()` value from 1.5.5 to 1.5.7 without changing the independent
  Robotweax project version or the 0.2 shared-library ABI line.
- Advertise SRT 1.5.7 in HSv5 and make the current API manifest track the
  immutable Haivision v1.5.7 tag commit
  `899348d8318eb9a3c5a5b6ec43c4a1114288773a`.
- Use Haivision SRT 1.5.7 as the primary reference-interoperability profile;
  retained 1.5.6 security and 1.5.5 backward-encryption lanes are explicitly
  version-scoped.

### Added

- Added the public `SRT_KM_S_E_SIZE = 6` key-management enum sentinel and the
  platform-correct `SYSSOCKET_INVALID` constant from the SRT 1.5.7 header
  surface. Preview-only AEAD declarations remain conditional.
- Added fail-closed tests for empty, unaligned, and noncanonical no-argument
  control payloads, including proof that rejected controls do not refresh peer
  liveness.

### Fixed

- Serialize ACKACK, KEEPALIVE, SHUTDOWN, congestion-warning, and PEERERROR
  controls with the required four-byte zero word. Incoming control payloads
  are now rejected before state or liveness changes unless they are nonempty
  and aligned to the SRT 32-bit word boundary; stricter type-specific checks
  still apply afterward.

### Performance

- Replaced per-sample send/receive buffer byte and timestamp-span scans with
  bounded incremental accounting, and skipped message-expiration scans when
  no buffered packet carries a deadline.
- Avoided the global readiness mutex and condition-variable broadcast when no
  epoll waiter is registered while preserving generation-based wakeups.

## 0.2.1 — 2026-08-29

Robotweax SRT 0.2.1 is a compatibility-preserving maintenance release focused
on public integration, packaging, documentation, examples, and runtime
correctness. It does not change the 0.2 ABI line, the exact 80-symbol public C
surface, the SRT 1.5.5 compatibility target, or any wire behavior.

### Added

- Added an opt-in shared-library `srt.pc` compatibility facade for unmodified
  FFmpeg builds while retaining `robotweax-srt.pc` as the canonical package.
- Added a pinned minimal FFmpeg build gate with unencrypted and AES-CTR
  MPEG-TS caller/listener smoke transfers and elementary-stream validation.
- Added public, public-header-only Message, File/Stream, and Broadcast/Backup
  Connection Group demos with deterministic cross-platform smoke tests.
- Added a standalone installed-package CMake consumer. Release package tests
  build and execute the exact source shipped in the documentation tree against
  both static and shared `RobotweaxSRT::srt` targets.

### Changed

- Restructured the installed documentation around public build, integration,
  testing, compatibility, limitation, and migration guides while moving
  internal research and planning material out of the public source package.
- Expanded adjacent Doxygen contracts on the exported C API, machine-readable
  MIT licensing on installed public sources, third-party provenance, DCO
  enforcement, and the prohibition on publishing Haivision binaries as
  Robotweax artifacts.
- Split and selectively scheduled reference-interoperability profiles so
  unrelated pull requests do not always run the complete reference matrix.
- Documented isolated installation, provider verification, and the initial
  FFmpeg qualification boundary.

### Fixed

- Initialized Winsock through the public SRT startup/socket lifecycle so clean
  Windows consumers no longer depend on unrelated process initialization.
- Deferred peer-shutdown epoll errors until TSBPD-buffered input is drained,
  preventing event-driven consumers from truncating a received stream tail.
- Enabled bounded linger on the public Connection Group demo members so its
  final protocol acknowledgement is drained before orderly shutdown.

### Performance

- Avoided scanning an empty receive window while producing statistics,
  reducing direct `srt_bistats` cost on connected sockets with no buffered
  receive data without changing counter or reset semantics.

### Compatibility and ABI

- The project version is `0.2.1`; the shared-library ABI remains `0.2` and the
  exact public C export set remains unchanged from `0.2.0`.
- `srt_getversion()` continues to report the independent SRT API compatibility
  target `1.5.5`; HSv5, AES-CTR defaults, and the opt-in AES-GCM contract are
  unchanged.
- The `0.2.0` AES-GCM release manifest remains the frozen authority for the
  extension contract. This maintenance release adds no Robotweax-specific API
  or protocol extension.

See `docs/release-notes-0.2.1.md`, `docs/compatibility.md`, and
`docs/limitations.md` for the complete release boundary.

## 0.2.0 — 2026-08-27

Robotweax SRT 0.2 establishes
the `0.2` pre-1.0 ABI line, retires positive HSv4 production establishment,
consolidates the supported handshake runtime on HSv5, and completes the
version-pinned AES-GCM extension profile without changing the default
Haivision SRT v1.5.5/AES-CTR compatibility surface.

### Included

- deterministic HSv5-only Caller, Listener, and Rendezvous establishment with
  explicit rejection of genuine legacy HSv4 attempts;
- provider-neutral authenticated encryption plus OpenSSL AES-128/192/256-GCM
  primitives, exact negotiation and key-material validation, authenticated
  DATA, immutable retransmission, replay protection, and acknowledged key
  rotation;
- opt-in AES-128-GCM Live/Message interoperability against the pinned capable
  Haivision revision over IPv4/IPv6 Caller/Listener and Rendezvous;
- Robotweax-to-Robotweax GCM File/Stream, protected-byte Row/Column/Matrix FEC,
  Broadcast/Backup groups, failover/replay, and binary/MPEG-TS timing gates;
- malformed key-material and authenticated-DATA fuzzing with fail-closed
  plaintext and output-buffer invariants;
- Linux, macOS, and Windows static/shared Release packages, installed C/C++
  consumers, exact 80-symbol exports, lifecycle tests, sanitizers, and fuzz;
- a complete migration guide, compatibility report, release manifest, and
  release notes with explicit capability boundaries.

### Compatibility and ABI

- The project version is `0.2.0` and its shared-library ABI line is `0.2`.
  Binary compatibility with the pre-1.0 `0.1` line is not promised, even
  though both releases currently export the same exact 80 C symbols.
- `srt_getversion()` continues to report the independent default API target
  `1.5.5`. `SRTO_CRYPTOMODE` is available only when the library and consumer
  are built with `ENABLE_AEAD_API_PREVIEW=ON`.
- Existing AES-CTR behavior remains the default and is covered by the full
  regression matrix. Explicit GCM never silently falls back to CTR.
- Positive HSv4 source and interoperability evidence remain available only in
  the immutable `v0.1.0` release.

### Known limits

- The released GCM extension is default-off and is not a claim of complete
  SRT 1.6 API or wire compatibility.
- Pinned-reference AES-GCM transfer evidence is AES-128 and Live/Message only.
  AES-192/256 reference transfer and pinned-reference Caller/Listener rotation
  remain unclaimed.
- Cross-implementation GCM File/Stream is rejected by the pinned reference's
  TSBPD requirement; encrypted mixed-reference groups are also unclaimed.
- The general 0.1 production-readiness limits remain: dedicated-host scale and
  performance targets, long-duration soak/fuzz campaigns, physical two-node
  faults, hardware timestamp evidence, and conservative partial API entries.

See `docs/release-notes-0.2.0.md`, `docs/migration-0.2.md`,
`docs/compatibility.md`, and `compat/robotweax-0.2-aead.json` for the complete
release boundary.

## 0.1.0 — 2026-08-26

The first evidence-backed development release of Robotweax SRT. It establishes
the `0.1` pre-1.0 ABI line while retaining Haivision SRT v1.5.5 source, ABI,
and behavioral compatibility as the selected public target.

### Included

- a compatible public C API and exact 80-symbol shared-library export boundary;
- HSv5 Caller, Listener, and Rendezvous establishment over IPv4 and IPv6;
- isolated legacy HSv4 interoperability profiles against checksum-pinned
  Haivision SRT v1.2.3;
- Message/Live and Stream/File transport with reliability, flow control,
  congestion control, TSBPD, statistics, epoll, callbacks, and file helpers;
- AES-128/192/256 HaiCrypt interoperability, acknowledged key rotation, and
  fail-closed hostile key-material handling;
- Row, Column, Matrix, Even, and Staircase FEC foundations with bounded memory,
  deterministic recovery, rollover, fuzz, and retained-chain evidence;
- Broadcast and weighted Main/Backup Connection Groups with bounded replay,
  encrypted local failover and late joining;
- bounded process-wide scheduling, completion, and deferred-close services;
- relocatable CMake and pkg-config packages, static and shared builds, exact
  versioned ABI validation, and installed C/C++ consumer tests;
- cross-platform builds and packages, sanitizers, fuzz smoke tests,
  deterministic protocol tests, and version-pinned interoperability checks.

### Compatibility boundary

- The project version is `0.1.0`, its shared-library ABI line is `0.1`, and
  `srt_getversion()` reports the independently selected compatibility target
  `1.5.5`.
- Compatibility claims apply only to the documented platform, role, direction,
  mode, option, and version-pinned peer profiles. Unqualified combinations are
  not implied by the presence of a public declaration.
- Conditional upstream preview APIs `SRTO_CRYPTOMODE` and
  `SRTO_MAXREXMITBW` are outside the selected v1.5.5 default profile and are not
  silently approximated.

### Known limits

- This release is not yet a universal production replacement for arbitrary
  `libsrt` deployments.
- Dedicated many-socket CPU, memory, thread, wakeup, throughput, and tail-
  latency scorecards on controlled hosts remain outstanding.
- Long-duration soak and adversarial fuzz campaigns, physical two-node fault
  evidence, hardware NIC/PHC timestamp validation, and oscillator-grade timing
  evidence remain outstanding.
- Some public functions and socket options remain conservatively partial for
  untested platforms, modes, or edge cases; that release used the then-current
  1.5.5 inventory, now superseded by `compat/srt-1.5.7-api.json`; the current
  boundary is summarized in `docs/api-compatibility.md`.
- Broader mixed-reference encrypted Connection Group failover and suitable
  external evidence for newer FEC/group combinations remain outstanding.

See `docs/compatibility.md`, `docs/api-compatibility.md`, and
`docs/limitations.md` for the supported and remaining-work boundary.
