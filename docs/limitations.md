# Known limitations

Robotweax SRT 0.2.7 is an evidence-backed pre-1.0 release with an explicit
support boundary. This document summarizes limits that application developers
and integrators should account for. Unsupported operations are intended to fail
with defined errors rather than being silently approximated.

## Version and ABI boundaries

- The project version is 0.2.7 and the shared-library ABI line is 0.2.
- Binary compatibility with the 0.1 ABI is not promised. Rebuild applications
  and dependencies against the 0.2 package.
- The default compatible API target and `srt_getversion()` value are 1.5.7;
  they do not report the Robotweax release number.
- Before 1.0, do not assume binary compatibility across minor ABI lines.
- The stable installed integration boundary is the public C API. Native
  source-tree C++ interfaces are not a promised shared-library ABI.

## Compatibility scope

Robotweax implements a substantial SRT 1.5.7-compatible surface, not every
behavior of every `libsrt` release. Some declarations remain available so an
application receives a defined unsupported-operation result. Do not infer
feature support only from the presence of a symbol or enum value; consult the
[API compatibility](api-compatibility.md) and [Socket options](socket-options.md)
documents.

Qualify the exact peer version, topology, transport mode, and encryption
profile used in production. Self-interoperability does not prove universal
third-party interoperability.

The primary compatibility evidence uses the official Haivision SRT v1.5.7
tag at commit `899348d8318eb9a3c5a5b6ec43c4a1114288773a`. A successful test
against that profile does not imply automatic compatibility with preview,
fork-specific, or future SRT extensions.

Pinned Haivision v1.5.5 and v1.5.7 file helpers can publish successful,
byte-complete size-to-EOF transfers before their sender statistics include the
final one to three packets. Qualification accepts this bounded statistics lag
only for those exact versions, only when payload files are byte-identical, and
never as evidence for missing transport data.

## HSv5-only connection establishment

Robotweax SRT 0.2 supports positive connection establishment through HSv5.
Positive HSv4 establishment is not supported and there is no automatic
downgrade. An integrated HSv5 peer must identify coherently as SRT 1.3.0 or
newer.

The initial HSv5 discovery exchange contains a version-4 induction value as
part of the protocol; applications must not interpret that field alone as a
successful HSv4 connection.

## Encryption boundaries

AES-CTR is the default security profile. It provides payload confidentiality
but does not authenticate the payload. Applications requiring authenticated
payloads should evaluate the released Robotweax 0.2 AES-GCM extension and its
peer compatibility.

AES-GCM is opt-in at build time through the historical
`ENABLE_AEAD_API_PREVIEW` flag. Default and GCM-enabled builds have different
header-visible contracts and must not be mixed. The extension is version-pinned
and does not imply a general SRT 1.6 compatibility claim.

Current positive cross-implementation GCM evidence is narrower than
Robotweax-to-Robotweax coverage. In particular, do not infer positive
cross-implementation GCM support for File/Stream, encrypted groups, every key
length, or every rotation profile. Review [Encryption](encryption.md) and test
the exact peer profile.

Key-material (KM) rollback protection is bounded. Known duplicate announcements
can be answered again without reinstalling old keys. Known key material must
retain its original salt, selector and cipher mode across current receive slots
and the retired-key history (four generations per key parity); relabelled
material is rejected. Requests containing only retired keys are rejected while
those generations remain in the history. A captured KM announcement older than
both histories is indistinguishable from a fresh announcement and can install
an old key, disrupting reception until the sender's next rotation. See
[Encryption and key rotation](encryption.md) for the bounded rollback rules.

Control packets, including KM, ACK, NAK, DROPREQ and KEEPALIVE, are not
authenticated by the protocol. AES-CTR provides payload confidentiality only;
AES-GCM additionally authenticates protected DATA packets. GCM authentication
does not extend to these runtime control packets.

File/Stream plus packet-filter FEC is not a supported combination. GCM payloads
also reserve a 16-byte authentication tag, reducing the application payload
budget relative to AES-CTR for the same MSS and address family.

## MSS and datagram size

`SRTO_MSS` accepts values from 76 through 1,500 bytes. Jumbo MSS values above
1,500 are not supported by the fixed receive-datagram storage. A truncated UDP
datagram is discarded as a whole and is not passed to packet decoding.

The application payload ceiling depends on:

- negotiated MSS;
- IPv4 versus IPv6 header overhead;
- Message versus Stream mode;
- AES-CTR versus AES-GCM;
- packet-filter recovery headers.

Do not use one fixed payload ceiling across all combinations. Query negotiated
settings and follow the calculations in [Socket options](socket-options.md).

## Platform-specific socket options

- `SRTO_BINDTODEVICE` is implemented through `SO_BINDTODEVICE` on Linux. It is
  unsupported on macOS and Windows and may require operating-system privileges.
- Winsock does not expose a settable IPv6 traffic-class option equivalent to
  `IPV6_TCLASS`. An explicitly configured IPv6 `SRTO_IPTOS` therefore fails on
  Windows; IPv4 TOS and IPv4/IPv6 TTL remain available.
- Native socket buffer sizes can be adjusted or doubled by the operating
  system. Robotweax reports its portable effective SRT values rather than
  promising identical kernel bookkeeping on all platforms.

Always test binding, dual-stack behavior, interface selection, and packaging
on every target operating system.

## Connection-group scope

The compatible group API supports Broadcast and Backup policies for documented
Live Caller/Listener use cases. The following are not supported compatibility
profiles in 0.2:

- balancing and multicast group policies;
- group Rendezvous;
- group File/Stream or FileCC;
- treating a group as one shared cryptographic session;
- inferring encrypted mixed-implementation group support without an exact
  peer qualification.

Members configured with `SRTO_MESSAGEAPI=false` are accepted by the group
option setter but do not take part in group receive; group I/O requires the
message API on every member.

When an explicitly closed member still holds complete unread messages, the
group copies them into bounded receive-only storage (8,192 packets,
11,927,552 payload bytes, 16 batches, 120 s unread lifetime; see
[Connection groups](connection-groups.md)). Exceeding a bound or expiry drops
retained receive data; allocation failure discards the affected batch. The
closed-member copy keeps the earliest complete messages within its packet and
byte budget and skips individually oversized messages. At group capacity, older
batches are evicted to make room for newer ones. Receive and readiness inspection
check expiry. The group advances past data no remaining path can supply and
counts the skipped packets in `pktRcvDropTotal`. This receive-side loss does not
break healthy members or the group send path. Haivision SRT discards a closed
member's unread data outright; Robotweax retention can preserve a bounded prefix
but cannot guarantee delivery after its limits are reached.

Group members retain per-path connection, security, and statistics state.
Applications own topology, link diversity, path health inputs, member
replacement, and recovery policy. A single-host test does not prove physical
path independence.

See [Connection groups](connection-groups.md) for option ownership and member
lifecycle rules.

## Packet-filter FEC scope

The built-in packet filter supports Row, Column, and Matrix FEC for documented
Live profiles. FEC cannot repair arbitrary loss patterns: recovery depends on
geometry, available parity, group expiry, and whether ARQ remains enabled.

Robotweax deliberately accepts `cols` values from 2 through 128. Haivision
v1.5.7 accepts a wider 16-bit geometry range, but mirroring that range would
allow a peer configuration to multiply decoder group, bitmap, XOR-buffer, and
reconstruction allocations. The 128-column ceiling is retained as bounded
resource protection and means that complete FEC option-range parity is not
claimed. The qualified common geometries remain interoperable.

Packet filtering reduces payload capacity and changes statistics. Applications
must validate expected loss bursts and overhead rather than enabling a filter
without a traffic-specific geometry. See [Packet-filter FEC](packet-filter.md).

## Live tail probes and retransmission counters

With Live congestion control, peer periodic NAK support, and ordinary ARQ
(`always`), Robotweax schedules one last-sent DATA probe when the sender RTO
expires with unacknowledged packets and no pending selective retransmission.
The probe recovers a lost final packet and exposes preceding gaps that the
receiver could not otherwise report. It is paced and uses the existing timeout
backoff; a cumulative ACK that clears the flight stops further probing.

An ACK delayed beyond that timeout can therefore cause a probe even when all
original DATA arrived. A nonzero `pktRetransTotal` with zero sender/receiver
loss counters is possible on a path with no DATA loss. The receiver can observe
an extra DATA datagram while delivering each message only once. Retransmission
counters alone are not proof of packet loss, and clean-path monitoring should
not assume they are always zero.

This is a deliberate recovery-policy difference from Haivision SRT v1.5.7 at
`899348d8318eb9a3c5a5b6ec43c4a1114288773a`. Source review of
[`srtcore/core.cpp`, `CUDT::checkRexmitTimer`](https://github.com/Haivision/srt/blob/899348d8318eb9a3c5a5b6ec43c4a1114288773a/srtcore/core.cpp#L12219)
confirms that its Live timer fallback is disabled when the peer advertises
periodic NAK support. No identical recovery policy is claimed. File mode,
peers without periodic NAK, and filter-controlled ARQ have separate fallback
rules. See [Live flight-tail recovery](testing.md#live-flight-tail-recovery)
for the deterministic regression and lost-tail delivery checks.

## Timing and media scope

Robotweax provides SRT pacing, TSBPD delivery, source-time metadata, and timing
measurement support. It is not a media parser, MPEG-TS muxer, PCR generator, or
general-purpose PCR repacer. Applications remain responsible for media clock
discipline and for generating coherent source timestamps.

Loopback timing evidence does not cover every NIC, kernel scheduler, virtual
machine, clock source, or wide-area network. Validate timing on representative
two-node hardware and define workload-specific limits.

## Performance and production qualification

The release does not constitute a universal throughput, socket-count, or
latency guarantee. Results depend on compiler, OpenSSL build, CPU topology,
payload distribution, MSS, encryption, FEC, socket buffers, and network
impairments.

Production users should add:

- representative two-node and multi-path tests;
- long-duration soak and reconnect campaigns;
- workload-specific sanitizer and fuzz coverage;
- dedicated-host throughput and scalability measurements;
- hardware or kernel timestamp validation where required;
- monitoring thresholds for loss, retransmission, delay, buffer use, and key
  state.

## Application responsibilities

Robotweax implements transport behavior; the application still owns:

- endpoint discovery and authorization;
- passphrase storage, rotation, and redaction;
- reconnect and backoff policy;
- connection-group topology and failover policy;
- media framing and timestamp generation;
- resource limits and admission policy;
- telemetry retention and privacy;
- deployment-specific production qualification.

Review the [Integration guide](integration.md) and [Testing guide](testing.md)
before declaring a deployment supported.
