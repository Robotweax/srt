# Robotweax SRT 0.3.2 — Multicast and Recovery Stabilization

Version 0.3.2 integrates the changes since [0.3.1](release-notes-0.3.1.md),
including multicast dispatch, optional ARQ-first recovery, valid-RTT clock
correction and early listener-capacity enforcement. Published artifacts and
their provenance are available from [GitHub releases](https://github.com/Robotweax/srt/releases).

## Versions and upgrade

Project version **0.3.2**, shared-library ABI **0.3**, compatible SRT API and
`srt_getversion()` **1.5.7**. The 85 C exports and existing public C layouts of
0.3.1 are preserved. Native source-tree C++ consumers must rebuild against
matching sources. Applications from ABI line 0.2 still require rebuilding.
New options require matching 0.3.2 headers and library.

## ARQ-first FEC

[ARQ-first](arq-first-fec.md) is a regular opt-in sender policy for Live Matrix
FEC with `arq:always`. It withholds parity when the measured RTT budget allows
retransmission inside the peer's receive delay. The separately enabled tail
probe can expose losses at the end of a burst. Defaults remain always-on parity
and no probes; neither option changes the filter string or handshake.

Recovery, Message and UDP bridge examples expose `--arq-first`. Real UDP tests
cover plaintext, CTR/GCM and controlled final-packet losses with measured RTT.
Missing RTT feedback and short cold-start bursts retain the documented limits.
The release makes no general bandwidth-saving or delivery-deadline guarantee.

## Experimental multicast dispatch

The Robotweax-to-Robotweax multicast group has one Listener sender and multiple
Caller receivers. Payload is sent once over IPv4 multicast; each receiver keeps
its own unicast control and recovery connection. Shared group CTR/GCM encryption
and optional shared-loss multicast repairs are supported within the documented
trusted-group boundary. FEC, Rendezvous, File/Stream and Haivision peers are
outside this profile.

The [MPEG-TS application](multicast-media.md) connects UDP encoders and players,
optionally records received media and exposes multicast counters. A sender
continues accepting messages while no receivers are present. Key progression
now follows those unused positions, so a later receiver can resume after many
rotations. Failed key generation preserves the previous key and stream position.
An empty open sender reports writable readiness without a receive/error event;
backpressure remains tied to the elected transmitter.

The [completed security source review](security-review-2026-11-04.md) includes
multicast and cryptography. Shared keys still provide neither sender identity
against other key holders nor cryptographic revocation on receiver departure.
Physical-network fan-out and scale require separate qualification.

## Timing and admission

TSBPD drift correction uses valid measured RTT rather than treating the initial
RTT default as a measurement. IPv6 multicast environment detection distinguishes
unsupported host capability from protocol failure.

Listeners reserve accept capacity before provisional socket allocation,
application admission callbacks and connection setup. Reservations cover queued
and in-flight accepts and are released on failure. Joining an already-published
multicast sender does not consume a new accept slot.

## Qualification and packages

Multicast qualification requires the lifecycle and epoll regressions plus
Plaintext/CTR/GCM media samples. Sample subprocess logs are retained even on
timeout. Native multicast and idle-readiness tests have separate bounded CTest
partitions; individual protocol assertions are unchanged.

Windows SDK qualification upgrades the published signed 0.3.1 installers and
validates installed documentation. Source archives, package recipes, exports and
exact-version consumers must identify the same frozen product. See the
[0.3.2 release contract](release-process-0.3.2.md) for publication gates and the
protected independent Windows rebuild.

Existing ACP1, compiled-provider, mobile-pacing and backend qualification limits
remain in their own guides. No new universal CPU, memory, throughput, WAN, bottle,
PPA or COPR qualification is implied by this release.
