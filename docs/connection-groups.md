# Connection Groups

Robotweax SRT 0.2.6 implements the Haivision SRT v1.5.7 Connection Group ABI
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
| Multicast group type | Protocol-managed multicast membership | Not supported |

The public `SRT_GFLAG_SYNCONMSG` bit value is preserved for ABI compatibility,
but the v1.5.7 profile accepts only zero flags. Unsupported or unnegotiated
group types are rejected; they are never downgraded to Broadcast or Backup.

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
Link-level values (RTT, bandwidth, buffers, per-member loss and
retransmission) remain per member socket. A group that has not connected yet
reports `SRT_ENOCONN`; a closed group reports `SRT_EINVSOCK`.

For a complete public-header-only example, see
[`robotweax_srt_group_demo`](../examples/README.md#connection-group-api-demo).
It covers endpoint configuration, Broadcast delivery, weighted Backup,
`SRT_MSGCTRL.grpdata`, `srt_group_data`, per-member statistics, and a
controlled failover observed through `SRT_EPOLL_UPDATE`.

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

## Group Membership extension

The HSv5 Group Membership record contains:

- a 32-bit group ID;
- an 8-bit group type;
- an 8-bit flags field; and
- a 16-bit member weight.

The record is decoded as part of the transactional HSv5 extension chain. A
response must identify a compatible mirror group. A changed mirror identity,
type, ISN, or unsupported flag is rejected before the connection is published.

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

A better-weight candidate may be exercised in parallel, but it becomes
authoritative only after it acknowledges group DATA and remains responsive for
one stability interval. Local queue acceptance alone is not proof of path
health. The same rule controls failover and failback.

`SRTO_GROUPMINSTABLETIMEO` is group-only. Its compatible default and minimum
are 60 ms; the effective interval also respects RTT and peer-idle constraints.
Set it on the Backup group before its first connection. Ordinary socket
getters/setters return `SRT_EINVOP`. A per-endpoint override through
`srt_config_add` is not supported and returns `SRT_EINVPARAM` immediately;
the coordinator currently uses one group-wide minimum for all links.

The [v1.5.7 option contract](https://github.com/Haivision/srt/blob/v1.5.7/docs/API/API-socket-options.md#SRTO_GROUPMINSTABLETIMEO)
identifies it as a group option. The reference also exposes individual link
configuration through `srt_config_add`; that override remains outside the
implemented profile here.

## Replay and failover

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

`SRT_EPOLL_UPDATE` reports a nonterminal membership change, such as a later
member joining or one path becoming broken while another remains usable.
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
- 256 bytes per option value.

These bounds exceed the documented v1.5.7 member-option requirements. Exceeding
them returns an error instead of growing memory without limit.

`srt_group_data` follows the public size-query and capacity contract over a
coherent member snapshot. `SRT_MSGCTRL.grpdata` is a current status snapshot;
it is not guaranteed to identify the exact path that delivered an already
deduplicated logical message.

## Compatibility limits

The 0.2 group profile is limited to Live-mode Caller/Listener operation.
Rendezvous groups, FileCC groups, striping, multicast, and arbitrary
application-defined group policies are not supported.

Connection Groups provide path management within their documented resource and
timing bounds; they do not by themselves prove physical path diversity,
independent failure domains, production failover latency, or lossless recovery
beyond retained replay history. Validate the intended network topology before
deployment.

See [Socket options](socket-options.md), [Protocol edge cases](protocol-edge-cases.md),
and [Known limitations](limitations.md).
