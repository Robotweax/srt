# Experimental standby RTT protocol foundation

Since 0.2.9 the library provides explicitly enabled standby RTT monitoring for
Robotweax-to-Robotweax Backup Live Message groups using Caller/Listener with
both TSBPD directions. The default is disabled. File, Broadcast, Rendezvous and
Striping are unsupported. Enabled reference-provider interoperability and WAN
behavior have not been qualified. This feature is absent from published v0.2.8
artifacts; discover the optional C API from the exact selected library.

Set `SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED` (`0x01000008`, bool) before connecting
the caller Backup group and before listening on the listener, which also needs
`SRTO_GROUPCONNECT=true`. Ordinary callers and listeners without group admission
cannot enable the profile at setup. Both peers must enable it; missing or
unsolicited support fails with `SRT_REJ_GROUP`, with no automatic discovery or
silent downgrade. Group and listener templates carry the option to new members. Endpoint
configurations (`srt_config_add`) reject it with `SRT_EINVPARAM`.
It cannot change after setup or in a listen callback. Disabling TSBPD/Message
mode or selecting a different transport/congestion profile while it is required
fails rather than leaving an ineffective option.

Existing RTT V1, `msRTT`, DATA/ACK cursors, RTT/NAK/RTO, KeepAlive, idle and group
send authority are unchanged. The separate measurement does not establish DATA
health, select a sender or replace the existing recovery DATA probe. Internal
C++ interfaces remain uninstalled and outside the stable public C ABI.

## Bilateral handshake identity

`HandshakeMachine::Configuration::require_standby_rtt` defaults to false.
Enabling it requires a nonzero local 128-bit nonce supplied by the transport
owner from its cryptographic random source. The caller must be a Backup group;
the listener must have a group negotiator. Both require Live Message mode and
both TSBPD flags. The native transport draws a fresh nonce for every new member from its existing
cryptographically seeded identity generator, failing closed if randomness is
unavailable. Reconnect does not reuse the negotiated identity.

The experimental configuration extension is `0x7f12`, advertised under the
existing configuration category bit. Its exact 40-byte content is:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | Version, network-order integer 1 |
| 4 | 4 | Reserved, zero |
| 8 | 16 | Caller nonce |
| 24 | 16 | Listener nonce |

Caller CONCLUSION offers its nonce and a zero listener nonce. A configured
listener answers with the unchanged caller nonce and its distinct nonzero
listener nonce. A missing capability, unsolicited capability, invalid profile
or wrong caller nonce is rejected with Group reason 15; malformed length,
reserved words, unsupported version and zero/equal pair values fail decoding.
There is no silent downgrade. An off/off handshake emits no new extension.
Retransmission preserves the offer; listener replay requires the original
offer and returns the same pair. Session-authentication AGREEMENT, when used,
must preserve the pair, but this does not authenticate the measurement control.

`0x7f12` is independently selected from the experimental namespace. The source
tree uses `0x7f10` for session authentication, `0x7f11` for path identity, and
`0x7f03/0x7f04` for authenticated key controls; no current own identifier
collides. This is not a standards registration or a reservation against future
third-party features. A future collision requires a versioned compatibility
decision, not reinterpretation of an existing measurement packet.

## Request and echo representation

Exactly 64 bytes including the ordinary 16-byte SRT control header; trailing
bytes and truncated packets are rejected. Control type is User-Defined
`0x7fff`, subtype `0x7f12`, type-specific information zero. Timestamp and socket
ID retain their ordinary envelope meaning; wire timestamps do not enter RTT.
The 48-byte payload is:

| Payload offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | Version 1 |
| 4 | 4 | Kind: 1 caller Request, 2 its Echo, 3 listener Request, 4 its Echo |
| 8 | 16 | Negotiated caller nonce |
| 24 | 16 | Negotiated listener nonce |
| 40 | 8 | Nonzero network-order probe ID |

An echo has the same length, identity, initiator and probe ID as its request.
The initiator marker separates simultaneous equal IDs in opposite directions.
An echo never triggers another echo. The transport owner must first validate
the established peer endpoint and destination socket before dispatching the
decoded message to the measurement instance. Nonces provide correlation, not
cryptographic authentication or DATA-delivery proof. DATA encryption and the
existing authenticated key exchange must not be treated as protection for this
new control subtype.

## Serialized measurement and budgets

The owner configures a valid negotiated pair, nonzero member generation, local
role and first due time. One probe may be prepared at a time. Only a successful
native UDP send completion starts its RTT clock; failed/backpressured sends
consume no sample. The next attempt is at least one second after preparation
and native acceptance. The owner calls `finish_send` for each immediate send
attempt and must not queue or retry these controls in the DATA/ACK FIFO.

Echo timing is local monotonic datagram ingress minus native send acceptance.
The owner records ingress before later inbox/lock work. A single early echo
can wait for send completion publication; unresolvable nonpositive timing is
discarded. Correlation checks pair, initiator, ID and member generation.
Missing, wrong, duplicate or late echoes cannot manufacture a fresh sample.
ID wrap disables further requests. No remote clock synchronization is needed.

Response deadline is at most two seconds after native acceptance, capped by the
already existing idle deadline passed at preparation. A response exactly at
deadline is expired. The provisional unsent slot also expires within two
seconds or the earlier idle deadline. Expiry retains the last sample and its
age; it does not break a connection or update an estimator. Age is unavailable
until a positive sample exists; freshness ends after three seconds. A backward
read before the last sample reports unavailable age.

`pause()` cancels an open probe when the member becomes active, a DATA probe or
otherwise ineligible. `terminate()` stops requests and replies while retaining
the sample for a still readable terminal handle. Reconfiguration clears the
sample history. The runtime cancels pending measurement at BROKEN, peer shutdown/error and
explicit close. Passive UDP retirement owns no measurement runtime. Cleanup
invalidates admission generations; delayed work cannot claim a replacement lease.

`StandbyRttBudget` is a fixed 256-slot admission table with generation-bearing
leases. Round-robin grants visit at most 256 slots. Request and echo grants are
separately spaced by at least 31,250 microseconds (32/s, burst one); the member
responder additionally permits at most one echo per second. The runtime uses one serialized process-wide budget and marks only due,
eligible members ready. The next fair lease is reserved for its polling owner;
a delayed claim also advances the claim interval, preventing a catch-up burst. No grant requires allocation, new threads or catch-up
work. Release/clear invalidate old leases without recycling serials or
replenishing send tokens. Budget denial is local resource pressure, not loss.

The compiled macOS arm64 layouts are 160 bytes per measurement and 2,336 bytes
per budget. Compile-time ceilings are 256 and 4,096 bytes respectively. These
numbers cover these objects, not total Runtime/Handshake/channel memory or
network performance. The runtime additionally bounds each admitted member state to 256 bytes and
the process service to 4,096 bytes, excluding allocator/ownership overhead and
the pre-existing Runtime/Handshake/channel state. Its service is dormant until
needed and adds no threads, queues or per-member timer owner. A retained terminal
runtime keeps its admission slot until destruction, so terminal history cannot
bypass the 256-state memory bound. Capacity exhaustion preserves the connection
but disables both request and echo work; the public snapshot reports it. The
slot becomes available after the retained handle/runtime is closed and retired.

## Evidence and integration boundary

The own implementation is independent of the public specification's ordinary
ACK/ACKACK RTT exchange. The public protocol source used for the design was
[the Internet-Draft at 461a1fea](https://github.com/Haivision/srt-rfc/blob/461a1fea85baaa4a928fa634d30c846dfd047ab8/draft-sharabayko-srt.md),
not third-party implementation code. This extension is an independently authored
experimental contract, not a requirement of that draft.

Deterministic tests use serialized packet bytes, injected monotonic times and
send outcomes. They cover malformed and duplicate extensions, negotiation
mismatch/replay, equal IDs in both directions, fast echo/completion ordering,
backpressure, old generations, deadline caps, retained sample age, overflow,
responder limits and full admission/fairness/reuse. The native runtime tests additionally cover the isolated receive path, endpoint
and destination admission, native send acceptance, backpressure, DATA/authority
pauses, request floods, process fairness/capacity, unchanged idle expiry, retained
history and handshake replay. Checked local group connections cover bilateral
activation and one-sided rejection; these are loopback tests, not WAN or matched
Network Lab qualification. Encryption/session-authentication profiles do not
turn the new control into authenticated traffic.

## Native measurement and snapshot

A request bypasses the production send FIFO and consumes an existing channel
send-attempt slot. Contention on the channel send lock also skips monitoring
without waiting for that lock. Its local timing begins immediately after successful native
UDP acceptance, not preparation or enqueue. Backpressure or a transient native
failure skips it without queuing/retrying; hard failures follow normal terminal
connection handling. The receive time is captured after native UDP receive,
before decoding, route lookup and runtime lock waiting. Both directions use
only their own local monotonic times; packet timestamps are not RTT evidence.

Requests pause for the selected sender, the pending recovery DATA probe, queued
or unacknowledged outbound DATA, pending native FIFO work, and one second after
accepted inbound or sent outbound DATA. Group metadata is checked with a short
nonblocking lock attempt: contention skips/cancels monitoring rather than
introducing a reverse lock wait or holding metadata over native I/O. Echo replies
remain bounded independently, never enter the FIFO and stop at the unchanged
local idle deadline or terminal boundary. Neither request, reply nor reception
refreshes liveness, statistics, RTT estimators or group authority.

Call `robotweax_srt_standby_rtt_data_v1` on a member handle with a caller-owned
`ROBOTWEAX_SRT_STANDBY_RTTDATA_V1`. Initialize `struct_size=sizeof(snapshot)` and
`abi_version=1`; supply the actual writable capacity. V1 is 48 bytes, reserved
words are zero and a larger buffer's tail is untouched. Invalid size/version/
null output fails with `SRT_EINVPARAM` without changing output. Unconnected or
CONNECTED-before-runtime-publication states return `SRT_ENOCONN`, closing
`SRT_ECONNLOST`, invalid/closed/group handles `SRT_EINVSOCK`. Connected and retained
BROKEN members are readable until their public close.

The flags are `NEGOTIATED`, `ADMITTED`, `OBSERVED`, `FRESH` and `TERMINAL`.
`ADMITTED` records admission, including retained terminal state. Disabled
connections have no flags or samples. A negotiated connection without a slot
reports only `NEGOTIATED` (plus `TERMINAL` when applicable) and outcome `CAPACITY`.
`rtt_microseconds` is a positive measured round trip, not a smoothed estimator;
`samples` counts accepted correlations, saturating at `UINT64_MAX`.
`age_microseconds=UINT64_MAX` means no usable sample age. `FRESH` means local age
at most three seconds; it is a display freshness rule, not proof of DATA health.
At 256 eligible members, the 32/s process budget can produce eight-second or
longer intervals, so honest stale snapshots are expected.

`outcome` reports `NONE`, `PENDING`, `MEASURED`, `TIMEOUT`, `SEND_SKIPPED`, `PAUSED`,
`ID_EXHAUSTED` or `CAPACITY`. Loss, budget denial, cancellation and terminal
transition retain the previous measurement and its increasing age; no zero
measurement or freshness is fabricated. Reads and statistics resets do not
initiate, reset or refresh probes. A new connection starts a new history.
