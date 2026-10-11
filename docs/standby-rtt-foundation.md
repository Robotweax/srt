# Experimental standby RTT protocol foundation

This source-tree foundation contains a bilateral handshake identity, a bounded
request/echo codec and a serialized measurement state machine. It is not an
enabled public socket feature: there is no socket option, runtime producer,
receive handler or public measurement getter in this stage. Ordinary compatible
sockets keep the feature disabled and reject a required measurement offer with
the existing Group rejection reason. The internal C++ interfaces are not
installed or covered by the public C ABI.

The intended first profile is Robotweax-to-Robotweax Backup Live Caller/Listener
with both TSBPD directions. File, Broadcast, Rendezvous and Striping are outside
this foundation's handshake configuration. Enabled reference-provider
interoperability has not been qualified. Existing RTT V1, `msRTT`, DATA/ACK
cursors, RTT/NAK/RTO, KeepAlive, idle and group send authority are unchanged.

## Bilateral handshake identity

`HandshakeMachine::Configuration::require_standby_rtt` defaults to false.
Enabling it requires a nonzero local 128-bit nonce supplied by the transport
owner from its cryptographic random source. The caller must be a Backup group;
the listener must have a group negotiator. Both require Live Message mode and
both TSBPD flags. The owner must generate a fresh nonce for every new member
connection and must not reuse an identity after reconnect.

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
sample history. Runtime close/cleanup and the passive channel-retirement path
must call the corresponding cancellation boundary when integrated later.

`StandbyRttBudget` is a fixed 256-slot admission table with generation-bearing
leases. Round-robin grants visit at most 256 slots. Request and echo grants are
separately spaced by at least 31,250 microseconds (32/s, burst one); the member
responder additionally permits at most one echo per second. A future runtime
owner must maintain one serialized process-wide budget and set only due,
eligible members ready. No grant requires allocation, new threads or catch-up
work. Release/clear invalidate old leases without recycling serials or
replenishing send tokens. Budget denial is local resource pressure, not loss.

The compiled macOS arm64 layouts are 160 bytes per measurement and 2,336 bytes
per budget. Compile-time ceilings are 256 and 4,096 bytes respectively. These
numbers cover these objects, not total Runtime/Handshake/channel memory or
network performance. Actual runtime fairness, flood handling and system
cleanup still need integration qualification.

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
responder limits and full admission/fairness/reuse. These are protocol-core
tests, not an enabled socket, WAN, authenticated-control or reference-provider
qualification. Follow-up work must connect this foundation to native routing,
one process budget, unchanged KeepAlive/idle bookkeeping, cancellation and a
separately versioned optional snapshot, then run matched Group regression.
