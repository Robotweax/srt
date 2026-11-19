# Robotweax SRT 0.3.4 — Epoll Wake, Setup Hardening and Experimental Profiles

Version 0.3.4 integrates the changes since [0.3.3](release-notes-0.3.3.md).
Release artifacts and their publication status are recorded in
[GitHub releases](https://github.com/Robotweax/srt/releases).

## Versions and upgrade

Project version **0.3.4**, shared-library ABI **0.3**, compatible SRT API and
`srt_getversion()` **1.5.7**. The 85 C exports and existing public C layouts of
0.3.3 are preserved, and one export is added: `robotweax_srt_epoll_wake`, for 86
in total. New socket options and the new call require matching headers and
library. Native source-tree C++ consumers must rebuild against matching sources;
their interfaces are not a supported shared-library ABI. Applications from ABI
line 0.2 still require rebuilding.

## Epoll wake

`robotweax_srt_epoll_wake(eid)` lets another thread end one `srt_epoll_wait` or
`srt_epoll_uwait` on an epoll instance without registering a system socket or
closing an SRT socket. A wait with events reports them first and leaves the wake
pending. A wait with nothing to report returns as an expired timeout would and
consumes the wake. Repeated wakes before consumption count once, and a wake is
not distinguishable from a timeout by return value. Headers that declare the
call define `ROBOTWEAX_SRT_EPOLL_WAKE_VERSION`. Programs that never call it see
no change. See [API compatibility](api-compatibility.md#optional-epoll-wake).

## Setup, group and file-receive hardening

A connection group retains at most `ROBOTWEAX_SRT_MAX_GROUP_MEMBERS` members,
pending and connected together. The limit is a build-time setting from 1 to
65536 with a default of 64; a member beyond it is refused. Deployments that
relied on larger groups must raise the setting when building.

Setup input is validated before cryptographic or metadata state changes. After
an attempt is bound, a packet whose peer cookie, socket ID or flow window differs
is ignored and the original attempt continues within its deadline, instead of
failing the attempt. Haivision induction-to-conclusion socket-ID changes and
early rejections before cookie binding are preserved. See
[rendezvous](rendezvous.md).

File receive keeps one file handle across network waits and delayed truncation,
so a rename after opening cannot redirect truncation or writes. A final symbolic
link or Windows reparse point is rejected. Regular files, new files and POSIX
character devices remain valid destinations; initial open failures return
`SRT_EWRPERM` immediately. Applications remain responsible for authorizing
parent directories. See [file mode](file-mode.md).

## Timing and performance

A channel with buffered DATA previously polled its socket every 2 ms. When no
datagram has arrived for 20 ms it now waits for its earliest protocol deadline,
capped at 10 ms, and relies on socket readiness for new input. Denser traffic, a
pending key exchange and an already-due deadline keep the 2 ms poll. This
changes wake-up timing for sparse traffic on every build. Measured on macOS
loopback at about 10 packets per second, receiver context switches fell from
about 5,000 to about 3,350 over 10 seconds; 40 to 950 packets per second were
unchanged within noise. No general throughput or CPU improvement is claimed.

Per-packet paths drop redundant zero-fills and a per-byte buffer-overlap test.
`ROBOTWEAX_SRT_ENABLE_LTO` is a new opt-in build option, and configuration
prints a notice when no build type is set.

In the default-off hybrid control profile, successful freshness refreshes are
scheduled from the confirmation origin instead of reply receipt. Delayed but
valid replies no longer let a confirmed policy expire between refreshes.

## Experimental profiles

These additions are default-off, use provisional identifiers and are outside
the supported security and interoperability boundary.

- **Null packet deletion.** `SRTO_ROBOTWEAX_NPD_POLICY` removes canonical
  MPEG-TS null packets at the sender and restores them bit-exactly at the
  receiver on individual Live Caller/Listener sockets, including encrypted DATA
  and FEC. Stock peers do not implement it: REQUIRED fails against them and an
  IF_OFFERED listener serves them with ordinary SRT. A non-OFF policy lowers the
  `SRTO_PAYLOADSIZE` ceiling by one byte. See the
  [NPD contract](null-packet-deletion.md).
- **Pinned-key peer authentication.** Builds with
  `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_PEERAUTH` add caller/listener
  authentication with pinned P-256 identities, implemented for OpenSSL only.
  **The cryptographic design has not been independently reviewed** and no
  support or security claim is made. Groups and rendezvous are not covered.
  See the [draft contract](peer-authentication.md).
- **Linux UDP batching.** Builds with
  `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_UDP_BATCHING` use `recvmmsg` and
  `sendmmsg`; `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_UDP_SEGMENTATION` adds
  `UDP_SEGMENT`. Selection is compile-time only. FEC, filter, group and
  experimental-controller connections keep the direct path. See the
  [UDP I/O backend draft](udp-io-backends.md) for measurements and their limits.

## Qualification and limits

The [release contract](release-process-0.3.4.md) requires frozen-source CI,
ecosystem integrations, provider, hybrid and peer-authentication matrices,
package consumers, upgrades from the signed 0.3.3 Windows SDKs, and one
protected rebuild and signing on the final tag.

The [2026-11-04 source review](security-review-2026-11-04.md) covers its recorded
older revision. The [0.3.4 delta disposition](security-delta-0.3.4.md) records the
focused follow-up for the changes since 0.3.3 and the retained limits;
final-source qualification is recorded separately.

Long-horizon compatible-mode KMREQ replay limits, shared multicast-key trust and
revocation limits, and experimental ACP1 and hybrid restrictions remain
documented in [security policy](../SECURITY.md), [encryption](encryption.md) and
[limitations](limitations.md). A lost final Live packet is still not guaranteed
to recover without later DATA. Loopback and virtual-host measurements do not
establish physical-network, WAN or general throughput guarantees. Historical
Homebrew bottles and external package repositories do not qualify new 0.3.4
distributions.
