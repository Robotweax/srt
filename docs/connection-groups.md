# Connection Groups

Robotweax SRT 0.3.1 implements the Haivision SRT v1.5.7 Connection Group ABI
for Live-mode Caller/Listener Broadcast and weighted Main/Backup operation.
Groups provide one public handle over several SRT connections. Members retain
independent crypto, transport-sequence, and congestion state.

**0.2.3 timing correction:** members share the group's timestamp origin and
receive TSBPD clock. This correction is not included in the published v0.2.2
tag; see the [0.2.3 release notes](release-notes-0.2.3.md), including the
remaining qualification limits. Update both Robotweax endpoints to obtain the
synchronized timeline; mixed old/new group timing is not qualified. Native C++ consumers
must rebuild because implementation layouts changed; the public C ABI is
unchanged by this correction.

This page is the application contract. The broader release boundary is in
[Compatibility status](compatibility.md).

## Supported group types

| Public type | Behavior | Status |
| --- | --- | --- |
| `SRT_GTYPE_BROADCAST` | Send one logical message on every usable member; receive one ordered copy | Supported |
| `SRT_GTYPE_BACKUP` | Select one authoritative member by weight and health; retain alternatives for failover | Supported |
| Balancing / striping | Split one logical stream across paths | Not supported |
| `ROBOTWEAX_SRT_GTYPE_MULTICAST` | One Listener sender, N Caller receivers; payload once over UDP multicast, control per receiver over unicast | Experimental, Robotweax-to-Robotweax; optional group encryption is unreviewed (see [Multicast dispatch groups](#multicast-dispatch-groups)) |

The public `SRT_GFLAG_SYNCONMSG` bit value is preserved for ABI compatibility,
but the v1.5.7 profile accepts only zero flags. Unsupported or unnegotiated
group types are rejected; they are never downgraded to Broadcast or Backup.
`ROBOTWEAX_SRT_GTYPE_MULTICAST` is declared after `SRT_GTYPE_E_END`, so every
reference enumerator and `SRT_GTYPE_E_END` itself keep their v1.5.7 values; a
Haivision peer rejects the type with `SRT_REJ_GROUP`.

## Listener group authorization

A remote group ID is a lookup key, not proof of membership. On a listener with
an admission callback, accepting a group connection now requires the callback
to set `SRTO_ROBOTWEAX_GROUPDOMAIN` to a nonzero `uint64_t` after application
authentication and authorization. Merely returning zero from the callback no
longer admits group connections. Existing group-aware callbacks must migrate.
Ordinary non-group callbacks are unchanged.

Inside the callback, `SRTO_ROBOTWEAX_PEERGROUP` returns the proposed remote
`int32_t` group ID (`SRT_INVALID_SOCK` for a non-group connection). Use it with
the authenticated application principal to authorize this particular join.
Assign stable domain values from trusted application state; never derive a
domain solely from the unauthenticated stream ID or proposed group ID. Distinct
security domains must have distinct values, including across bonded listeners.

The first member fixes the mirror group's domain. Later members presenting the
same remote group ID on the listener/bond must match it before membership,
replay or fan-out is installed. The domain setter is only valid inside a group
listen callback, and zero is rejected. Both new options are local metadata;
no extension is sent on the wire and compatible peers need no changes.

Without a callback, an explicitly group-enabled listener remains one shared
trust domain. Do not share such a listener between mutually untrusted clients.
Session key authentication does not itself authorize group membership or
bind the group fields; shared-passphrase holders are not distinct principals.

## Public API surface

The selected API includes:

- `srt_create_group`;
- `srt_groupof` and `srt_group_data`;
- `srt_create_config`, `srt_config_add`, and `srt_delete_config`;
- `srt_prepare_endpoint` and `srt_connect_group`;
- group-handle `srt_connect`;
- listener admission through `SRTO_GROUPCONNECT`;
- multi-listener `srt_accept_bond`;
- group information in `SRT_MSGCTRL`; and
- `SRT_EPOLL_UPDATE` for nonterminal membership changes.

Group handles share the `SRTSOCKET` integer namespace and carry the public
group mask. Applications must use public predicates and APIs rather than
inspecting private handle bits.

An empty group reports `SRTS_BROKEN`. Closed handles remain invalid for the
rest of the active runtime generation, so a stale application event cannot
accidentally acquire a later group or member.

`srt_bstats` and `srt_bistats` accept a group handle and report the group's
own counters, as libsrt does: messages sent through the group are counted
once (`pktSentUnique*`, `byteSentUnique*`), not once per member; messages
delivered by the group receiver (`pktRecvUnique*`, `byteRecvUnique*`); and
gaps the group receiver skipped because every member had dropped them
(`pktRcvDrop*`, `byteRcvDrop*`, sized with the smoothed received payload or
the 1316-byte Live default before the first delivery). `msTimeStamp` counts
from the first connection's start; `clear` resets the interval counters.

Unlike libsrt, a group row also reports `pktRetrans*` and `byteRetrans*`: the
retransmissions of all its members, including members that have since closed.
Each member's cumulative count is folded in once, when the group row is read
and when the member closes, so the group total never decreases and a group
`clear` does not touch member intervals. A removed member's accounting baseline
survives linger and the final send drain; their remaining retransmissions are
committed when runtime retirement completes. During asynchronous linger, that
tail appears after retirement rather than through the removed member row.
A group row does not report receive loss: sequences missing on one path are recovered or dropped per member, and
`pktRcvDrop*` already counts those that no member delivered.

Other link-level values (RTT, bandwidth, buffers, per-member loss) remain per
member socket. A group that has not connected yet reports `SRT_ENOCONN`; a
closed group reports `SRT_EINVSOCK`.

A Backup member's `pktRcvDrop*` can increase when an ordered DROPREQ retires
history already confirmed on another path. Other members may still hold those
messages for group delivery at their TSBPD deadline. Inspect group delivery
and group drop counters alongside member counters when diagnosing failover;
a member-local retirement count does not by itself establish group payload loss.

For a complete public-header-only example, see
[`robotweax_srt_group_demo`](../examples/README.md#connection-group-api-demo).
It covers endpoint configuration, Broadcast delivery, weighted Backup,
`SRT_MSGCTRL.grpdata`, `srt_group_data`, per-member statistics, and a
controlled failover observed through `SRT_EPOLL_UPDATE`.

## Direct adoption of app-bound UDP sockets

Version 0.3.1 adds the Robotweax-only
`robotweax_srt_connect_group_acquire` function. It accepts the usual endpoint
records plus a matching, writable array of native UDP sockets. Every descriptor
must already be bound to a nonzero local port and remain unconnected. Configure
the platform's network/interface selection before the call. Each new member
adopts its corresponding descriptor through the same path as `srt_bind_acquire`;
there is no anchor SRT socket, address-based channel lookup, descriptor duplication
or replacement UDP socket. Existing group modes, scheduling and wire negotiation
are unchanged.

Prepare each endpoint with a null source; the adopted socket supplies the actual
local binding. Explicit source addresses or ports are rejected before adoption.
The usual group defaults and endpoint overrides apply before adoption, including
UDP buffers, TTL/TOS and supported device settings. As with `srt_bind_acquire`,
these SRT options can update the native socket. Platform interface selection
outside those options is retained on the original descriptor.

```c
/* wifi_fd and cellular_fd are separately created, network-selected, bound,
   unconnected UDP sockets. remote is a populated sockaddr_in. */
SRT_SOCKGROUPCONFIG endpoints[2];
UDPSOCKET sockets[2] = {wifi_fd, cellular_fd};
for (int i = 0; i < 2; ++i)
    endpoints[i] = srt_prepare_endpoint(NULL,
        (const struct sockaddr*)&remote, sizeof(remote));

int result = robotweax_srt_connect_group_acquire(group, endpoints, sockets, 2);
/* Inspect every slot regardless of result. A slot set to (UDPSOCKET)-1
   transferred to SRT; the application must never use or close that descriptor.
   An unchanged slot remains application-owned and must be closed or retried
   by the application. endpoints[i].errorcode describes that member's result. */
```

Ownership transfers at successful adoption, before group enrollment and
connection setup. A later failure closes that member's descriptor and leaves
its array slot invalid; failure does not return ownership. A failed adoption
leaves the slot unchanged. Endpoint validation failures take no descriptors.
A blocking call can time out after starting members, just as `srt_connect_group`
can; use the returned member IDs to inspect or close pending connections.
Endpoint option snapshots remain caller-owned and may be deleted after return.
Closing a member or group releases its adopted channel under the normal
[UDP ownership contract](api-compatibility.md#addressing-and-udp-ownership).

Pass distinct, exclusively application-owned native sockets. Repeated descriptor
values are rejected before adoption. Descriptor aliases and sockets already
owned by SRT are not valid inputs. Existing Linux native reuseport restrictions
remain in force; direct adoption does not turn a listener-only channel into an
outgoing path. A subsequent call must contain fresh valid descriptor slots;
do not resubmit the invalidated slots from an earlier call.

The extension adds one C symbol without changing `SRT_SOCKGROUPCONFIG` or other
existing public layouts. It does not expand the supported group-mode profile.
Simultaneous Wi-Fi/cellular routing, mobile background execution and network
migration still require platform/device qualification.

## Endpoint preparation and connection

`srt_prepare_endpoint` accepts IPv4 or IPv6 destination and optional source
addresses. It validates pointer, length, family, and same-family source/
destination requirements before storing a result in `SRT_SOCKGROUPCONFIG`.
A zero source port requests operating-system selection.

`srt_connect_group` applies the endpoint's bounded option snapshot before the
member starts connecting. Each member receives its application token and
weight. All members of one group use one logical 31-bit initial sequence.

Group member defaults use the same fixed-schema configuration, value validation,
normalization and PRE/POST stages as ordinary sockets. Before the first
connection, a group can configure the supported bandwidth, latency, packet
filter, payload, transport, buffer, network and security options. New members
inherit the complete configuration, including members added later. Endpoint
options apply afterwards and can override those defaults. `SRTO_CONGESTION`,
`SRTO_RENDEZVOUS`, `SRTO_GROUPCONNECT` and `SRTO_BINDTODEVICE` setters remain
unsupported on group handles. Accepting an option does not expand the Live-mode
group data-plane profile described below.

After a group opens, PRE options fail with `SRT_ECONNSOCK`. POST options, such
as `SRTO_MAXBW`, `SRTO_INPUTBW`, `SRTO_MININPUTBW`, `SRTO_OHEADBW`,
`SRTO_LOSSMAXTTL`, `SRTO_SNDDROPDELAY` and `SRTO_DRIFTTRACER`, update current
members and the defaults for future members. Existing members are validated
before an update is applied. A member closing concurrently can cause the call
to fail after another member accepted the update; callers should check errors
and member values when changing options during teardown. `SRTO_MAXBW` is a
per-member limit, not an aggregate bandwidth cap.

Group transports require Live mode and TSBPD. Setting `SRTO_TRANSTYPE=FILE`
or disabling `SRTO_TSBPDMODE` fails with `SRT_EINVPARAM` before changing the
member template or any connected member. `SRTO_MESSAGEAPI=false` retains its
accepted option/getter behavior, but it does not provide usable Live group I/O;
use Message API for group traffic.

Group I/O synchronization, send/receive timeouts and Backup stability timeout
remain owned by the group. `SRTO_STATE` returns the aggregate group state.
Other supported getters use the first member, including negotiated latency,
key state and an accepted Stream ID. An empty group returns its configured
defaults. Mirror groups seed those defaults from the listener, then use their
accepted member's effective values. Group getters are representative values;
they do not assert that endpoint overrides are identical on every member.

`SRTO_LATENCY` sets both latency directions and its getter returns receive
latency. Stream IDs are limited to 512 bytes; their getter needs space for an
additional terminating NUL and returns the length excluding that terminator.

The first connect on a new group waits for the first usable member when the
`SRTO_RCVSYN` option requires blocking behavior. Later members may
complete in the background. Passing a group handle to ordinary `srt_connect`
uses the same path and returns the created member socket ID, not zero.

Connection callbacks are snapshotted per new member. Replacing the group
callback affects future members and does not rewrite callback state already
owned by an in-progress member.

When Broadcast sends encounter both a failed member and congested surviving
members, the failed member is closed once. Nonblocking sends report
`SRT_EASYNCSND`; blocking sends wait for surviving capacity up to `SRTO_SNDTIMEO`.
A send-buffer shortage does not close a surviving member.

## Listener admission

Group admission is disabled by default. Set `SRTO_GROUPCONNECT=true` on the
listener before `srt_listen` to accept a valid Broadcast or Backup request.
Otherwise the request is rejected with `SRT_REJ_GROUP`.

The first connected member publishes one mirror-group handle through
`srt_accept`. Later members with the same peer group identity and type join that mirror
without another accept result. The wire ISN can advance as traffic continues.
Each direction uses its own group sequence cursor: a late mirror member starts
sending at the mirror's current send cursor, while a late caller member starts
receiving at the caller group's current receive cursor.

`srt_accept_bond` forms an explicit domain from multiple listeners. Members of
one peer group may then arrive through different listeners in that same domain.
Listeners that were never bonded remain isolated. Merging two already distinct
bond domains fails with `SRT_EINVOP`.

A member may be admitted before the application first calls `srt_accept_bond`.
The mirror it created moves into the bond when the call forms it, so later
members of that peer group on any bonded listener join the same mirror. If two
bonded listeners had each already created a mirror for one peer group, one of
them joins the bond and the other stays bound to its own listener.

On the caller, the first member that receives a listener response binds the
group to that mirror identity. Later members answered by a different mirror are
rejected with `SRT_REJ_GROUP`. The binding becomes permanent once a member's
connection is established. Until then, if every member that observed the mirror
has failed, the next member may bind the group to the mirror the listener
answers with instead.

## Group Membership extension

The HSv5 Group Membership record contains:

- a 32-bit group ID;
- an 8-bit group type;
- an 8-bit flags field; and
- a 16-bit member weight.

The record is decoded as part of the transactional HSv5 extension chain. A
response must identify a compatible mirror group. A changed mirror identity,
type, ISN, or unsupported flag is rejected before the connection is published.

## Optional receiver-side path labels

The default group profile keeps application tokens local. Release 0.2.9
adds explicitly enabled, bilateral [group path identifiers](group-path-identifiers.md)
with a separate versioned metadata getter. This extension is absent from the
published v0.2.8 artifacts and is not Haivision-compatible when enabled.

## Broadcast semantics

A Broadcast send assigns one logical packet sequence, message number,
application source time, boundary, order, and TTL to all usable members.
Each member independently:

- maps source time into the group's common timestamp origin;
- applies its own negotiated cipher and key selector;
- performs its own congestion and flow-window control; and
- produces member-local wire bytes.

The send succeeds when at least one path accepts the logical message. A path
that cannot preserve the common message identity after partial success is
isolated rather than allowed to diverge silently.

Group receive publishes one complete ordered copy and discards equivalent
copies from other members. A member joining after earlier traffic is rebased to
the current logical ordering point and receives only subsequent traffic.

## Main/Backup semantics

Backup selects exactly one authoritative connected member using descending
weight and a stable socket-ID tie-break. A healthy current path remains sticky.

A better-weight candidate may be exercised in parallel. Qualification uses
send-buffer progress plus responsiveness over a stability interval. The cursor
comes from the first retained send sequence; local TLPKTDROP can advance it,
so this is not proof of remote DATA acknowledgement. Hard failure can permit
immediate fallback without completing a probe. Local queue acceptance alone
is not proof of path health.

Since 0.2.9, a pending fallback probe to a lower-priority member
is canceled at the next group send decision when the current authority becomes
responsive again. This check precedes candidate qualification, even if the
candidate has already completed its interval and acknowledged DATA. Priority
uses the same weight/socket-ID order as selection. A later response timeout
starts a new probe with a new interval and DATA boundary; the canceled probe's
qualification time is not reused. Probes toward a higher-priority member still
follow normal failback qualification while the current path is responsive.
Cancellation does not retract DATA already queued on the candidate. This rule
adds no post-outage hold-down or continuous-health guarantee; responsiveness is
the existing recent-response test, which also accepts keepalives.

**Release boundary:** The per-endpoint `SRTO_GROUPMINSTABLETIMEO` override
described below is available from 0.3.1. Published v0.3.0 supports only the
group-wide minimum and rejects this endpoint override with `SRT_EINVPARAM`.
See the [0.3.1 release notes](release-notes-0.3.1.md).

`SRTO_GROUPMINSTABLETIMEO` is group-only. Its compatible default and minimum
are 60 ms, and its fixed accepted range is 60–5000 ms; the effective interval also respects RTT and peer-idle constraints.
Set it on the Backup group before its first connection. Ordinary socket
getters/setters return `SRT_EINVOP`. For a caller Backup member, add an
`int32_t` `SRTO_GROUPMINSTABLETIMEO` to its endpoint's `SRT_SOCKOPT_CONFIG`
with `srt_config_add` before `srt_connect_group`. The endpoint value replaces
the group minimum for that member; endpoints without it inherit the group
minimum, including later additions. The value is copied at connection setup.
It controls both active-path response expiry and candidate qualification,
subject to the same RTT, peer-latency and peer-idle bounds. Invalid sizes or
values outside 60–5000 ms fail immediately with `SRT_EINVPARAM`.
Applying this override to a Broadcast endpoint fails that endpoint with
`SRT_EINVPARAM`; it does not create a member.

Listener-side configuration is unavailable: ordinary listener sockets reject
this option and accepted mirror groups are already open. This differs from
the reference listener inheritance and peer-idle-dependent maximum. Configure
caller Backup groups before opening them; do not assume mirrors can be tuned.

The [v1.5.7 option contract](https://github.com/Haivision/srt/blob/v1.5.7/docs/API/API-socket-options.md#SRTO_GROUPMINSTABLETIMEO)
identifies it as a group option and exposes individual link configuration
through `srt_config_add`. Robotweax retains its fixed range and caller-side
configuration boundary; this change does not claim complete option parity.

## Replay and failover

An inactive member can retain an old unacknowledged tail while another member
advances the group's replay-retirement boundary. If that boundary passes the
inactive member's send cursor, the member retires only the group-confirmed
prefix already retired by the group and sends an ordered sequence-only DROPREQ
covering its old tail and
the skipped gap. It then replays any outstanding group history before fresh
DATA. Control-queue backpressure leaves its buffered data and cursor unchanged.
This synchronization does not count as a member response or qualify promotion;
ordinary probe qualification still requires buffer progress and the
responsiveness interval, subject to the hard-failure fallback above.

The group retains a bounded plaintext history for unacknowledged logical
messages. When a replacement becomes authoritative, replay preserves the
original sequence, message, boundary, order, TTL, and absolute application
source time, then encodes the timestamp and creates member-local ciphertext.

Replay is limited to:

- 8,192 packets; and
- 11,927,552 payload bytes.

If a required gap has left retained history, sender-side failover fails
explicitly instead of replaying a discontinuous logical stream. On receive,
negotiated TLPKTDROP may retire an unrecoverable gap. The group advances past
it only after no member can deliver the expected sequence, including complete
messages buffered on a terminal member until their TSBPD deadline. This also
preserves complete messages behind a terminal member's pending head gap while
its receiver-drop deadline has not arrived, even if a replacement starts at a
later sequence.

Closing or breaking one member does not terminate a group while another usable
member exists. Applications may add a distinct replacement member. A broken
member socket is not recycled into a new connection. Runtime shutdown is
published by group receive, group-state queries, and `srt_group_data` without
requiring a separate member-state query. A BROKEN member remains available
for draining buffered messages; an explicitly closed member is detached from
the membership list, including while its transport finishes asynchronous linger.
Before detaching an explicitly closed member, the group copies its complete
unread messages into receive-only storage. Payload, sequence and message numbers,
and the shared TSBPD clock remain available after the member transport is released.
Adding a replacement cannot make these messages ready earlier or skip them.

This storage is limited per group to 8,192 packets, 11,927,552 payload bytes and
16 retained batches. Duplicate message ranges do not consume another batch.
Unread batches expire 120 seconds after member closure; receive and readiness
inspection check expiry. The bounds are enforced by discarding retained
data, oldest batch first (a single message that can never fit is discarded on
its own), and expiry or an allocation failure discards the affected batch.
When copying a closed member, the earliest complete messages within the packet
and byte limits are kept; an individually oversized message is skipped, and
aggregate overflow discards the remaining tail. The copy never consumes the
member's receive buffer.
Discarded data is receiver-side loss: the group skips past it to the lowest
sequence a remaining member can supply and counts the skipped packets in
`pktRcvDropTotal`, exactly as for a gap no member can supply. Retention never
breaks the group or its send path, matching the reference implementation,
which discards a closed member's unread data outright. Closing the group
releases this storage immediately. Retention preserves complete messages
already received locally; it cannot recover incomplete messages or packets
that reached no member.
Terminal members do not mask a pending replacement's CONNECTING state.

## Source time and TSBPD

The shared-clock behavior in this section is the 0.2.3 correction noted above,
not the behavior of the published v0.2.2 tag.

One logical group message owns one absolute application source time. Group
connections use one local timestamp origin for handshake, DATA, and control
packets, including members that connect later. Connection timeout budgets
start at each connection attempt, not at that shared timestamp origin.

On reception, members share one TSBPD timebase, timestamp-wrap state, and
slewed drift estimate. This clock drives both actual release eligibility and
the `SRT_MSGCTRL.srctime` delivery timestamp. An independently delayed member
handshake must not replace the established group clock. The clock survives
the retirement of its first member. The group's buffering delay is the maximum
negotiated receive delay seen by its clock; adding a member cannot shorten it.

Replay retains the absolute application source time and converts it through
the destination member's timestamp origin. Do not substitute timestamps from
an independently established, non-group connection.

This contract keeps duplicate suppression, TSBPD release, and application
timelines continuous across connections established at different moments. See
[Live timing](live-timing.md).

## Encryption

Pre-connection group or endpoint configuration supports passphrase, PBKEYLEN,
enforced encryption, KM refresh rate, KM preannouncement, and peer idle timeout
within the documented option rules.

Every member owns independent:

- cipher negotiation;
- transmit and receive keys;
- acknowledged rotation;
- replay window;
- receiver socket identity; and
- immutable ciphertext.

The AES-GCM extension is available to Robotweax groups only when the library
and consumer are built with `ENABLE_AEAD_API_PREVIEW=ON`. It is default-off.
Cross-implementation encrypted-group behavior outside the matrix in
[Compatibility status](compatibility.md) is not claimed.

## Synchronization, timeouts, and readiness

The group owns `SRTO_SNDSYN`, `SRTO_RCVSYN`, `SRTO_SNDTIMEO`, and
`SRTO_RCVTIMEO`. Member configuration must not apply these independently.

With asynchronous receive enabled on an empty connected group, receive returns
`SRT_EASYNCRCV`. A synchronous receive uses `SRTO_RCVTIMEO` and reports
`SRT_ETIMEOUT` when the deadline expires.

A later mirror-member join raises `SRT_EPOLL_UPDATE` on the accepting
listener socket, not the group handle. A member breaking while another remains
usable raises UPDATE on the group. Caller-side joins do not raise UPDATE;
query `srt_group_data` when a fresh membership snapshot is required.
Failure of the last nonterminal member exposes terminal `IN|OUT|ERR` readiness
once the logical receive path is drained. After peer SHUTDOWN, accepted messages
retain their TSBPD deadlines and become readable before terminal ERR is exposed.
A subscription to only `OUT|ERR` cannot drain that pending data and may continue
to wait. Subscribe to `IN` as well and consume the remaining messages, or close
the group explicitly when the application chooses to abandon them.
`srt_epoll_wait` does not consume UPDATE; use `srt_epoll_uwait` when subscribing
to that event.

Group operations never hold the membership lock across member I/O or
application callbacks.
Each member's epoll state is sampled under one runtime lock, including send
capacity, terminal state and delayed SHUTDOWN delivery. Group refresh still
resolves and scans the current members; it does not retain a readiness cache
across membership or runtime changes.

## Configuration limits

A member configuration owns a copy of its values; it does not retain caller
pointers. Robotweax limits one configuration to:

- 32 option entries; and
- 512 bytes per option value.

These bounds exceed the documented v1.5.7 member-option requirements. Exceeding
them returns an error instead of growing memory without limit.

`srt_group_data` follows the public size-query and capacity contract over a
coherent member snapshot. `SRT_MSGCTRL.grpdata` is a current status snapshot;
it is not guaranteed to identify the exact path that delivered an already
deduplicated logical message.

On connected members, `SRT_GST_RUNNING` reports a running send path or a
receive path that has accepted payload DATA. A redundant receive path becomes
running even if another member delivers the logical message first. Reads,
duplicate retirement, statistics interval resets, and Backup send-path idling
do not erase that receive activity. A connected member without either kind of
activity remains `SRT_GST_IDLE`; terminal socket states take precedence and
report `SRT_GST_BROKEN`. Control packets, rejected peers, and undecryptable
payloads do not activate the receive path. Snapshots refresh activity from
member runtimes without requiring an application receive call. This implements
the separate send/receive directions described by the
[public v1.5.7 member-state contract](https://github.com/Haivision/srt/blob/v1.5.7/docs/API/API-functions.md#srt_memberstatus).

## Multicast dispatch groups

Broadcast and Backup are *bonding* groups: one signal between two endpoints
over several links. A multicast dispatch group is the other category the SRT
draft reserved wire type 4 for and never implemented: one sender on the
Listener side, any number of Caller receivers, each with its own unicast SRT
connection for control, and the payload sent once to a UDP multicast group.
Receivers join and leave at any time; the sender does nothing per receiver
beyond the handshake. The feature is experimental and Robotweax-to-Robotweax.

The [live multicast audio/video sample](multicast-media.md) demonstrates the
public API with an MPEG-TS encoder, multiple receivers, recording, and playback.

Sender (Listener):

- set `SRTO_ROBOTWEAX_MCAST_GROUP` (a `sockaddr_in`/`sockaddr_in6` naming a
  multicast address and port) before `srt_bind`; this enables dispatch
  admission. `SRTO_ROBOTWEAX_MCAST_IFACE` selects the outgoing interface
  (IPv4 address, IPv6 scope index) and `SRTO_ROBOTWEAX_MCAST_TTL` the hop
  limit (default 1);
- install a listen callback. For a multicast proposal (`SRTO_GROUPTYPE`
  reads `ROBOTWEAX_SRT_GTYPE_MULTICAST`) it must set
  `SRTO_ROBOTWEAX_GROUPDOMAIN` and `SRTO_ROBOTWEAX_MULTICAST` (a 1..512 byte
  stream key). One dispatch group exists per listener or bond, key and
  domain; returning without the key rejects the join;
- `srt_accept` returns the dispatch group once, with the first admitted
  receiver. Later receivers join it and surface as `SRT_EPOLL_UPDATE`.
- `srt_send*` on the group encodes each message once per member buffer so
  ACK, NAK and unicast retransmission keep their per-receiver meaning, but
  only the first live member transmits the original, to the multicast group
  with the dispatch group handle as destination socket ID, from the
  Listener's own socket. The others record the packet as sent. A send with
  no receivers advances the stream and succeeds; `srt_recv*` is `SRT_EINVOP`.
- The dispatch group stays `SRTS_CONNECTED` with zero members while the
  listener is open; this is the defining difference from bonding, where an
  empty group is `SRTS_BROKEN`.
- Every DATA packet of a dispatch member, original or repeat, carries the
  dispatch group handle as destination socket ID; receivers route that ID
  on their multicast socket and on their control link.

Receiver (Caller):

- `srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST)` and `srt_connect_group`
  with exactly one endpoint and weight 0; a second endpoint is `SRT_EINVOP`.
  `SRTO_ROBOTWEAX_MCAST_IFACE` on the group overrides the Listener's hint;
- the Listener's conclusion handshake carries a 60-byte `multicast_dispatch`
  extension (type `0x7f14`, magic-guarded) with the group address, the
  destination socket ID to filter on, the live sequence to start at and the
  hop limit. The receiver joins the group on a dedicated socket and routes
  datagrams that carry that ID and the Listener's source address into its
  one member session; everything else on that socket is dropped before
  decoding. `srt_recv*` reads that member; `srt_send*` is `SRT_EINVOP`;
- closing the group leaves the multicast group before the control link ends.

Encryption: set `SRTO_PASSPHRASE` (and optionally `SRTO_CRYPTOMODE`,
`SRTO_KMREFRESHRATE`, `SRTO_KMPREANNOUNCE`) on the dispatch listener and the
same passphrase on every receiver. The sender uses one traffic key and salt
for the whole group and rotates by group sequence; each receiver still gets
the key through its own unicast KM exchange, and a receiver without the
passphrase, or with another mode or key length, is refused at admission. A
member's `SRTO_SNDKMSTATE` reports `SRT_KM_S_SECURED` once its receiver
holds the group key; DATA to that member is recorded only from then on. See
[one traffic key per group](encryption.md#multicast-dispatch-groups-one-traffic-key-per-group)
for the mechanism, the nonce argument and the trust boundary.

Repeats, pacing and readiness:

- every repeat is unicast to the receiver that asked, from that member's
  own buffer. With `SRTO_ROBOTWEAX_MCAST_REXMIT_THRESHOLD` set to *n* (1..16,
  POST on the sender group, default 0) the sender keeps a bounded tally of
  repeats per sequence: once *n* distinct receivers asked for the same packet
  within a couple of round trips, the member reaching *n* repeats it once on
  the multicast path. Only after UDP accepts that repair may another receiver's
  first request be suppressed. A repeated request from the same receiver uses
  unicast, including when the multicast repair was lost. A transient local
  multicast repair failure falls back to the member's unicast retry path.
  After a multicast attempt, window expiry restores unicast until the bounded
  tally entry is evicted; failed repairs do not restart a multicast retry loop;
- `SRTO_MAXBW` set on the sender group reaches every member, so it caps the
  transmitting member's pacer and with it the multicast path, as well as each
  member's unicast repeats. There is no separate aggregate cap;
- `SRT_EPOLL_OUT` on the sender group follows the elected transmitting
  member's readiness. That member must accept the payload before suppressed
  members can accept it. A full transmitter buffer blocks a synchronous send
  or returns `SRT_EASYNCSND` for a nonblocking send without advancing the
  group sequence. A terminal transmitter is retired before electing its
  replacement. Send success means local queue acceptance, not delivery;
- `SRTO_ROBOTWEAX_MCAST_STATS` (read-only, `ROBOTWEAX_SRT_MCAST_STATS_V1`)
  reports on the sender the packets and bytes sent on the multicast path, the
  repeats among them, the unicast repeats skipped and the live receiver
  count; on a receiver the datagrams its multicast socket received and those
  dropped before decoding because they did not carry the dispatch ID from the
  announced sender. The libsrt-shaped group row (`srt_bstats`) keeps its
  documented meaning and counts each message once.

Not supported in this release, and refused at admission rather than
half-working: packet-filter FEC on the dispatch listener, Rendezvous,
File/Stream mode, FileCC, several endpoints on a receiver, and
interoperability with Haivision libsrt, which has no implementation of this
type.

## Compatibility limits

The 0.2 group profile is limited to Live-mode Caller/Listener operation.
Rendezvous groups, FileCC groups, striping, and arbitrary
application-defined group policies are not supported. Multicast dispatch
groups are an experimental Robotweax extension with the limits above.

Connection Groups provide path management within their documented resource and
timing bounds; they do not by themselves prove physical path diversity,
independent failure domains, production failover latency, or lossless recovery
beyond retained replay history. Validate the intended network topology before
deployment.

See [Socket options](socket-options.md), [Protocol edge cases](protocol-edge-cases.md),
and [Known limitations](limitations.md).
