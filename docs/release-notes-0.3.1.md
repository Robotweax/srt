# Robotweax SRT 0.3.1 — Integrated Runtime and Resource Hardening

Version 0.3.1 integrates the changes on main since
[published 0.3.0](release-notes-0.3.0.md). Obtain published source and signed
Windows SDK downloads, checksums and tag provenance from
[GitHub releases](https://github.com/Robotweax/srt/releases).

## Versions and upgrade

Project version **0.3.1**, shared-library ABI line **0.3**, compatible public
SRT API and `srt_getversion()` **1.5.7**. The 84 published 0.3.0 C exports and
existing C layouts are preserved; `robotweax_srt_connect_group_acquire` is the
85th additive export. Applications using the new function require 0.3.1 or newer.
Native source-tree C++ consumers must rebuild against matching sources. Moving
from ABI line 0.2 still requires rebuilding applications and dependencies.
OpenSSL/AES-CTR remains the default; GCM and session authentication retain their
explicit opt-ins and documented limits.

## Connection setup and admission

Caller, Rendezvous and accepted sockets install their transport runtime before
publishing CONNECTED. Asynchronous observers and group senders can therefore use
the connected transport immediately. Rejected-child metadata tests synchronize
with retirement and require cleared metadata throughout the interval, then an
invalid handle. These changes address [#360](https://github.com/Robotweax/srt/issues/360)
without weakening the path-ID send or metadata-clearing assertions.

Setup runtime construction is serialized with route promotion. Epoll validates
subscriptions before consuming readiness events. Per-child session authentication
is verified during admission; application authorization and group-domain rules
remain in force.

## Resource, file and capture hardening

Rejected-message membership uses a construction-sized bounded index, and sparse
timestamp refresh uses bitmap lookup. Packet-path rejection allocates no new
index storage and preserves healthy-peer progress on shared channels. At an
8,192-slot receive capacity, the indexes add approximately 321 KiB of storage.
Sparse payload pools also reduce unused allocation; these changes do not provide
a deployment-scale memory or throughput guarantee.

Asynchronous linger admits at most 256 deferred sockets and 256 MiB of combined
configured send/receive payload capacity. Terminal runtimes retire promptly.
Budget exhaustion falls back to immediate close and can abandon outstanding data.
The byte budget measures configured payload storage, not RSS. Synchronous linger
is unchanged. See [File mode](file-mode.md#eof-shutdown-and-linger).

`srt_recvfile` validates arguments before opening the destination. Positive
offsets preserve bytes outside the received range; offset-zero replacement starts
after the first payload arrives. An existing file survives failure before delivery.
The destination requires read/write access; a missing file can be created empty
while waiting, and later failures can leave partial writes. Use application-level
temporary-file replacement when transactions are required.

The remote interoperability harness accepts a direct, non-set-id `tcpdump`
executable, fixed 192-byte snapshots, 100,000 packets and a hard inherited 32 MiB
per-file limit. Custom argv and privilege-changing wrappers must migrate.
Cleanup/artifact failures fail the run. These are bounded diagnostic captures,
not a sandbox or host-wide disk quota. See the
[remote harness](https://github.com/Robotweax/srt/blob/main/interop/README.md).

## Groups and applications

The [direct group-adoption API](connection-groups.md#direct-adoption-of-app-bound-udp-sockets)
takes separately app-bound UDP descriptors with per-endpoint ownership results.
It retains the endpoint ABI, group scheduling and existing wire behavior.
Caller Backup members can override `SRTO_GROUPMINSTABLETIMEO` per endpoint,
within 60–5000 ms; absent overrides inherit the group minimum. Version 0.3.0
supports only the group-wide setting.

The [application samples](../examples/applications/README.md) cover contribution
media, recovery/File transfers and expiring Sensor delivery, with membership and
console diagnostics. Library worker names support host CPU attribution. Samples
and names do not establish production media, power or performance acceptance.

## Explicit experimental additions

Default-off compiled congestion-controller and packet-filter providers use strict
bilateral selection, bounded private state and terminal failure handling.
The source-tree XOR8 filter has real-UDP recovery and ARQ-fallback regressions.
These private interfaces are not an installed plugin ABI or a general aggregation,
multicast or production security contract. See
[controllers](experimental-controllers.md), [filters](experimental-filters.md)
and [ownership contracts](packet-filter-extensions.md).

Opt-in mobile timer pacing and the iPhone harness remain experimental and need
exact-device qualification. ACP1 remains default-off, independently unreviewed
and outside the supported security profile. Neither this release nor its tests
close the [ACP1 review requirement](https://github.com/Robotweax/srt/issues/190)
or the [legacy replay limitation](https://github.com/Robotweax/srt/issues/112).

## Packaging and retained limits

Windows SDKs include the complete version-matched public library documentation
and test upgrades from the published signed 0.3.0 installers. The final-tag
workflow independently rebuilds all twelve variants once in the protected
signing environment, tests both installers and signs only that artifact's bytes.
It preserves source/run/hash binding and post-signing installation checks.

Qualification is exact-source and profile-specific. See the
[release contract](release-process-0.3.1.md) and published workflow/release evidence.
The earlier 0.3.0 comparative-resource exception does not qualify this source.
No new CPU, wakeup, RSS, throughput, universal WAN or transfer-deadline claim is
made. Live final-loss behavior, file drain/error semantics, group authorization
requirements and crypto replay limitations remain documented in
[compatibility](compatibility.md), [limitations](limitations.md) and
[encryption](encryption.md). No new Homebrew bottle, PPA, COPR or customer
application distribution is implied by the library release.
