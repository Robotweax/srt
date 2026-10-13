# Robotweax SRT 0.2.9 — Group Recovery and Observability

Status: **published on 2026-10-13**.
Release and downloads: [v0.2.9](https://github.com/Robotweax/srt/releases/tag/v0.2.9).
Final source, complete media, package and signed-installer gates passed.
See the [completed acceptance record](release-process-0.2.9.md).

Project version: **0.2.9**. Shared-library C ABI line: **0.2**.
Compatible SRT API and `srt_getversion()`: **1.5.7**. OpenSSL/AES-CTR remains
the default. AES-GCM and MAXREXMITBW retain their explicit build-time opt-ins;
session authentication remains disabled by default. BCrypt remains an
experimental Windows backend. Source-tree C++ consumers must rebuild.

## Backup recovery and member lifetime

Inactive Backup members can resume after the group has cumulatively retired
an older prefix, without replaying globally acknowledged payload or creating
a receiver sequence gap. Failed probes advance to the next eligible path;
recovery of the current higher-priority path cancels an inferior probe.
This preserves the existing weight and negotiated stability rules.

Idle-expired members publish their terminal state immediately and attempt one
shutdown notice. After automatic failed-member close, only the native UDP
channel may remain passively reserved for one local idle budget, capped at
30 seconds and 256 distinct channels. Traffic never renews that deadline;
cleanup releases it immediately. A non-reusable endpoint rebind can conflict
until release, and admission/resource failure falls back to immediate release.
This is bounded peer-cleanup protection, not continued protocol participation
or retention of a closed handle's statistics.

No new configurable hold-down, automatic reconnect or RTT-based member
selection policy is introduced. See [connection groups](connection-groups.md)
and [Backup acknowledgements](compatibility.md#backup-member-acknowledgements).

## Versioned observations and path identity

- Accepted group handles expose `SRTO_GROUPTYPE`. Receiver member activity
  records accepted DATA independently of application reads; it is not a
  last-receive age, DATA-health or delivering-path guarantee.
- Connected caller sockets report the local connection address captured at
  setup, retaining the bind address if resolution fails. This is not ongoing
  routing or public NAT-address discovery.
- Optional bilateral group path labels are exposed through
  `robotweax_srt_group_path_data_v1`. They are disabled by default and provide
  endpoint-local correlation, not path authentication or NAT migration.
- `robotweax_srt_rtt_data_v1` separates normal RTT observations, source and
  local acceptance age from an initial estimate. A peer estimate's original
  measurement age remains unknown.
- `robotweax_srt_group_send_state_v1` reports the local Backup sender's selected
  authority and generation. DATA counter activity and receiver observations
  remain separate from this sender decision.

These are additive versioned C getters. The 0.2.9 export snapshot retains all
previous public exports; existing structure layouts are preserved. The
development and 0.2.9 export inventories agree. See
[socket options](socket-options.md), [path labels](group-path-identifiers.md)
and [public API compatibility](api-compatibility.md).

## Optional Standby RTT monitoring

`SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED` explicitly enables a bilateral
Robotweax-to-Robotweax Backup Live Message Caller/Listener extension with both
TSBPD directions enabled. One-sided or unsupported setup fails; it never
silently falls back. Monitoring is disabled by default and is incompatible
with peers that do not negotiate it when required.

The separate `robotweax_srt_standby_rtt_data_v1` snapshot reports negotiation,
capacity, measurement outcome, RTT, age and freshness. Measurement traffic
does not renew ordinary idle liveness or drive the normal RTT estimator,
DATA recovery, congestion control or Backup selection. DATA, selected-sender
and recovery-probe activity pause initiation. The public outcome records the
last attempt; it is not a continuously updated pause flag.

The service bounds retained states and request/echo rates. Capacity admission
failure preserves the connection without fabricated measurements. Its control
packets are correlated but unauthenticated, including when DATA encryption or
session authentication is enabled. A control roundtrip does not establish
DATA-path health, available bandwidth or production readiness. See the exact
[Standby RTT contract](standby-rtt-foundation.md).

## Listener, rejection and Windows SDK changes

Listener handshake handoff uses bounded extraction and preserves existing
input validation. Linux native-socket adoption permits explicitly configured
reuseport listeners, rejects aliases of an already adopted socket and retains
the documented listener/group scope. Asynchronous caller rejection completion
is paced at 10 ms; applications still need bounded retry/backoff rather than
a tight reconnect loop. See [connect variants](connect-variants.md).

Same-backend Windows SDK upgrades reuse the existing installation directory,
support same-version repair and reject downgrade or relocation. A verified
inventory protects user-owned files and removes only unchanged obsolete SDK
files. Backend coexistence and independent rebuild-before-signing remain
required. The final SDK workflow passed both installers, actual signed 0.2.8
upgrade/repair/rejection/coexistence, protected independent reconstruction,
trusted publisher/timestamp checks and tests of the exact signed executables.
Draft and public release downloads match the qualified signed artifact byte
for byte. The release includes the OpenSSL and BCrypt installers, post-signing
`SHA256SUMS` and `tag-provenance.json`.

## Delivery, qualification and remaining limits

Live retransmission semantics are unchanged: later DATA reveals sequence gaps
and permits NAK recovery; a final missing packet without a subsequent gap is
not guaranteed to recover. Complete finite transfers use File mode and its
timeout recovery and documented drain/error handling.

The [final tag CI](https://github.com/Robotweax/srt/actions/runs/38365080090)
passed 52 jobs including full media integrations and Required CI; one optional
Live-timing diagnostic was skipped. Package managers, Linux packages and the
final signed SDK passed at the same tag commit. The acceptance record links
their exact runs, source pins and downloaded-byte digests.
Selected resource/300-second loopback and scoped Group/RTT evidence retains
its declared workload and source limits. No universal WAN, throughput,
lossless abrupt-close or automatic hold-down guarantee follows from a fixed
profile. The native Homebrew ARM64 bottle passed separate qualification in
[Tap #8](https://github.com/Robotweax/homebrew-tap/pull/8); maintainer review
and publication remain pending.

The compatible profile's long-horizon KMREQ replay limitation remains tracked
in [#112](https://github.com/Robotweax/srt/issues/112). Broader authenticated
runtime control remains the design-review subject of
[#190](https://github.com/Robotweax/srt/issues/190), not a shipped ACP1 profile.
DATA GCM and optional session authentication do not authenticate all runtime
controls. Consult [known limitations](limitations.md),
[encryption](encryption.md) and [compatibility](compatibility.md).

## Group callback migration reminder

The 0.2.8 requirement remains in 0.2.9: a group-aware listen callback must set a
nonzero `SRTO_ROBOTWEAX_GROUPDOMAIN` on the provisional socket after admission
authorization. A Haivision-style callback that only returns zero rejects group
members. See [migration guidance](api-compatibility.md#group-admission-callback-migration-since-028).
