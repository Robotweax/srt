# Robotweax SRT 0.2.8 — Runtime, Recovery and Security Hardening

Status: **release candidate in preparation; not published**.

Project version: **0.2.8**. Shared-library C ABI line: **0.2**.
Compatible SRT API and `srt_getversion()`: **1.5.7**. OpenSSL/AES-CTR remains
the default. AES-GCM and MAXREXMITBW retain their explicit build-time opt-ins;
optional session authentication remains false by default. BCrypt remains an
experimental Windows backend. These version and profile axes are independent.

## Live and reliable delivery

Live senders whose peers advertise periodic NAKs no longer retransmit DATA
solely because an ACK is late, following the documented Haivision 1.5.7 policy.
Later DATA reveals gaps and enables paced NAK recovery. A final lost Live
packet, or a lost burst followed by a source pause, is not guaranteed to recover.
File mode retains NAK and timeout recovery, including the final packet, without
Live expiry. Applications needing a complete finite transfer must use File
mode and its documented drain/error handling.

Sender too-late-drop deadlines now start at first transmission, rather than
initial queueing. A delayed key-rotation response permits bounded continued
use of the current transmit key before new DATA pauses. Optional
`ENABLE_MAXREXMITBW` adds protected-byte retransmission budgeting without
changing its default-off policy. See [Live/File recovery](limitations.md#live-recovery-with-periodic-naks),
[sender drop semantics](sender-drop-semantics.md), [encryption](encryption.md)
and [socket options](socket-options.md).

## Explicit Robotweax extensions

The [Control profile](control-profile.md) provides reliable ordered application
messages through FileCC without Live expiry. The [Sensor profile](packet-filter.md)
provides its separate low-latency FEC-only contract. Both require explicitly
matching peers and versioned admission; neither silently replaces Live or File.
Explicit GCM can authenticate their DATA in matching extension-enabled builds.
Control application messages are distinct from transport runtime controls.

[Session authentication version 2](session-authentication.md) binds initial and
runtime key material to fresh session identities and authenticated counters,
and authenticates FEC parity before decoder admission. It requires explicit
activation on both Caller/Listener peers, a passphrase and enforced encryption.
It has no automatic fallback, does not support Rendezvous and is not
Haivision-compatible when enabled. It does not authenticate all handshake
fields or ordinary ACK/NAK/shutdown traffic. CTR still lacks DATA integrity;
GCM remains a separate choice. This is not the broader ACP1 design proposal.

## Runtime and bounded resource work

Scheduler shards, indexed setup routing and sparse receive readiness reduce
unnecessary runtime bookkeeping while retaining bounded work and storage.
NAK processing and deferred peer-drop continuations are bounded per service
turn. Sender metadata is compacted without reducing payload capacity, window
sizes or counter widths. These changes do not establish a universal throughput,
CPU, connection-count or latency guarantee.

The candidate includes fixes for asynchronous connect failure diagnostics,
Backup-group ACK handling, callback retirement, idle liveness and short-outage
progress. Group scope and retained-data bounds remain documented in
[connection groups](connection-groups.md). Direct source-tree C++ consumers must
rebuild; their implementation ABI is not the installed public C ABI contract.

## Input validation and build integrity

The candidate hardens crypto admission budgets, checked buffer-size conversions,
FEC input validation, handshake/profile admission and receive/transmit key
identity separation. Private temporary test directories and escaped demo
terminal output protect local test/example use. CI actions use immutable refs.
The Windows SDK signing pipeline independently reconstructs installers from
reviewed source before signing and verifies final signed artifacts. Pipeline
changes alone are not qualification of new installers.

## Release qualification

Qualification is **pending**. The [release process](release-process-0.2.8.md)
tracks the immutable source, exact-head CI, complete FFmpeg/GStreamer/VLC/OBS
suite, OpenSSL/BCrypt profiles, package-manager/Linux package checks and final
signed Windows SDK assets. Historical 0.2.7 results do not qualify this candidate.
No 0.2.8 download, installer, bottle or published tag is claimed here.

The Security Cloud scan started during preparation targets the development
baseline. Its result must be reviewed against the candidate's product changes;
a running scan is not security acceptance. Preserve original test failures,
including timing-dependent cases; a successful retry does not explain them.

## Remaining security and deployment limits

- [Issue #112](https://github.com/Robotweax/srt/issues/112) remains open for the
  ordinary compatible profile: bounded key histories do not provide lifetime
  KMREQ replay protection. Optional session authentication is a distinct profile,
  not a change to that default. [Issue #190](https://github.com/Robotweax/srt/issues/190)
  tracks broader ACP1 design/cryptographic review; no ACP1 profile ships here.
- AES-CTR provides confidentiality only. DATA GCM does not authenticate all
  runtime controls. Session authentication also has explicit coverage limits;
  independent cryptographic review remains a separate requirement.
- Exact peer/version/mode/profile qualification is required. Sensor, Control,
  session authentication, encrypted groups and GCM extensions must not be
  advertised as universally interoperable with Haivision peers.
- Loopback/short lab evidence does not qualify physical NICs, arbitrary WANs,
  long-duration operation or all operating systems. Repeat relevant performance,
  soak, reconnect and multipath campaigns for the intended deployment.
- Clean package coexistence/removal is not upgrade/rollback evidence. No new
  PPA/COPR, vcpkg registry, all-platform bottle or third-party application binary
  distribution is implied.

See [known limitations](limitations.md), [compatibility](compatibility.md) and
[testing](testing.md) for the supported contracts and qualification boundaries.
