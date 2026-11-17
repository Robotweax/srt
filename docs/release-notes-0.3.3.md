# Robotweax SRT 0.3.3 — Hybrid Multicast and Transport Hardening

Version 0.3.3 integrates the changes since [0.3.2](release-notes-0.3.2.md).
Release artifacts and their publication status are recorded in
[GitHub releases](https://github.com/Robotweax/srt/releases).

## Versions and upgrade

Project version **0.3.3**, shared-library ABI **0.3**, compatible SRT API and
`srt_getversion()` **1.5.7**. The 85 C exports and existing public C layouts of
0.3.2 are preserved. New socket options require matching headers and library.
Native source-tree C++ consumers must rebuild against matching sources; their
interfaces are not a supported shared-library ABI. Applications from ABI line
0.2 still require rebuilding.

## Multicast profiles and hybrid media

Independent-source IPv4 multicast separates the fixed multicast source from the
per-receiver control connections. The opt-in coordinated sender owns original
emission, canonical recovery history and an aggregate wire budget. It requires
a positive `SRTO_MAXBW`, supports at most 64 receivers, and reports detached
members through group data. Detachment of an established member breaks its
unicast connection when the detachment is published. Source-failure recovery
retains its separately documented session exception. See
[connection groups](connection-groups.md).

The separate [experimental hybrid control](experimental-hybrid-control.md)
profile requires explicit ACP1 and HYBRID_CONTROL build options. Both remain
default-off. It adds pairwise authenticated startup, bounded membership recovery,
unicast fallback and multicast return with confirmed policy and overlap coverage.
It remains outside the supported security and Haivision interoperability boundary.

The [multicast media sample](multicast-media.md) accepts `--hybrid` in a static
enabled build, uses distinct receiver credentials, and reports path transitions
and controller budgets. Fallback uses a local live catch-up policy: it can discard
unread buffered media and reports skipped sequence positions. This is not a
lossless failover or decoder-continuity guarantee. Ordinary/shared builds retain
the existing sample and reject hybrid options clearly. No public hybrid C API is
introduced.

For ordinary multicast with positive group `SRTO_MAXBW`, unicast repeats now
share a cap of four times that rate instead of relying only on independent member
pacers. The cap is charged at actual UDP submission and blocked repair scans
preserve multicast recovery opportunities. Groups without a positive rate retain
their previous behavior; the coordinated profile has its own budget.

## Cryptography and lifecycle corrections

Delayed DATA is routed across receive-key replacement by accepted sequence
position. Retired generations expire before sequence reuse. GCM authenticates
against eligible generations where a companion generation was entirely lost;
CTR retains the documented ambiguity policy because it has no payload integrity.
Failed authenticated opens preserve ciphertext/tag input and never publish
unauthenticated output, including overlapping destination buffers.

Listener-close work cannot be starved by overdue timers. Independent multicast
socket profiles are revalidated at listen commit, and superseded routes are
retired only when their binding still matches. Linux epoll payloads are copied
before use to avoid references to packed, misaligned fields.

On Windows the runtime scheduler holds a one-millisecond timer period for its
lifetime. Build with `ROBOTWEAX_SRT_WINDOWS_TIMER_PERIOD=OFF` to leave timer
resolution to the application. This can affect pacing and power use; it is not
a sub-millisecond scheduling guarantee. Mobile pacing remains separate.

## Qualification and limits

The [release contract](release-process-0.3.3.md) requires frozen-source CI,
ecosystem integrations, provider and hybrid matrices, package consumers, signed
0.3.2 Windows SDK upgrades, and protected final-tag rebuilding/signing. Hybrid
soak provenance verifies that the build belongs to the recorded source checkout.
Slow lifecycle cases have separate serial 30-second process budgets; test
assertions and individual protocol deadlines are preserved.

The [2026-11-04 source review](security-review-2026-11-04.md) covers its recorded
older revision, not all 0.3.3 changes. The [0.3.3 delta disposition](security-delta-0.3.3.md) records the focused
follow-up and retained limits; final-source qualification is recorded separately.

Long-horizon compatible-mode KMREQ replay limits, shared multicast-key trust and
revocation limits, and experimental ACP1 restrictions remain documented in
[security policy](../SECURITY.md), [encryption](encryption.md), and
[limitations](limitations.md). Loopback qualification does not establish physical
multicast/IGMP, WAN, fan-out, decoder latency, or general throughput/resource
guarantees. Historical Homebrew bottles and external package repositories do not
qualify new 0.3.3 distributions.
