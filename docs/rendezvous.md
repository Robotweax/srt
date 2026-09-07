# Rendezvous

Robotweax SRT implements HSv5 Rendezvous as a simultaneous-open state machine
for two peers that both bind locally and connect to each other. It follows the
behavior described in section 4.3.2 of the
[SRT Internet-Draft](https://datatracker.ietf.org/doc/html/draft-sharabayko-srt-01#section-4.3.2)
and the released Robotweax compatibility contract.

## When to use Rendezvous

Rendezvous is useful when neither endpoint can operate as a conventional
long-lived Listener, but both know the other's reachable UDP endpoint and can
start a simultaneous connection attempt.

It does not provide NAT traversal signalling, endpoint discovery, relay
allocation, authentication of peer addresses, or automatic reconnection. An
application must provide those facilities where required.

## Public API

Configure one SRT socket as follows:

1. set `SRTO_RENDEZVOUS=true` before binding;
2. set all other pre-connection options;
3. bind the socket explicitly to its local UDP endpoint; and
4. call `srt_connect` with the peer endpoint.

`srt_rendezvous` performs the compatible set-option, bind, and connect sequence
in one call.

`srt_listen` and `srt_accept` are invalid on a Rendezvous socket. Both peers
must use the same address family; mixed IPv4/IPv6 Rendezvous is rejected before
handshake traffic.

Blocking and nonblocking connect follow the same terminal success, timeout,
rejection, epoll, callback, and last-error contracts as ordinary Caller
connection establishment.

## Handshake roles

Both peers begin by sending WAVEAHAND. Once both cookies are known, a wrapping
32-bit cookie comparison assigns exactly one protocol Initiator and one
Responder. These roles are protocol-local and do not imply application sender
or receiver direction.

The Initiator owns HSREQ and optional KMREQ generation. The Responder owns HSRSP
and optional KMRSP generation. Both start orders and both resolved cookie roles
are valid.

An identical-cookie contest is rejected because it cannot produce unique
roles. A changed peer cookie or contradictory role progression is also
rejected.

## Loss and duplicate handling

The handshake uses bounded retry and overall deadlines. It tolerates loss or
duplication of WAVEAHAND, CONCLUSION, HSREQ/HSRSP, KMREQ/KMRSP, and AGREEMENT
within those limits.

After one peer is locally connected, a duplicate valid CONCLUSION can trigger
the final AGREEMENT again so the other peer can complete. A Responder waiting
for final confirmation may also accept a valid connected-party DATA or control
packet as evidence and process that first packet after the runtime attaches.

Malformed, cross-generation, or contradictory packets cannot partially apply
peer metadata or crypto state.

## Supported transport modes

Robotweax SRT 0.2 supports Rendezvous for:

- IPv4 and IPv6;
- Live/Message with LiveCC and TSBPD;
- File/Stream with FileCC and TSBPD disabled;
- clear or AES-128/192/256 CTR encryption;
- the default-off Robotweax AES-GCM extension within its documented matrix;
- Row, Column, and Matrix FEC with Live/Message; and
- blocking and nonblocking application operation.

Connection Groups do not use Rendezvous in the v1.5.7 compatibility profile.
File/Stream plus FEC is invalid.

## Encryption

Both endpoints must configure compatible passphrase, key length, enforced
encryption, and cipher-mode policy before connecting. The resolved Initiator
and Responder roles determine key-material message direction, not application
payload direction.

AES-CTR is the default compatible profile. AES-GCM requires both the library
and consumer to be built with `ENABLE_AEAD_API_PREVIEW=ON`; explicit GCM never
falls back to CTR. Authentication completes before a received packet reaches
reliability or the application.

Key rotation, retransmission, duplicate suppression, and packet-sequence
rollover use the same per-direction state as Caller/Listener connections.

See [Encryption and key rotation](encryption.md).

## Timing and source time

Rendezvous does not change the Live timing model. `SRT_MSGCTRL.srctime` is an
absolute application source time in the `srt_time_now()` monotonic domain. Each
peer maps it to its negotiated 32-bit connection timestamp and TSBPD release
deadline.

Protocol Initiator/Responder roles do not change timestamp ownership. MPEG-TS
PCR remains application media metadata; Robotweax never derives SRT timing from
payload bytes. See [Live timing](live-timing.md).

## Errors

Common configuration and runtime errors include:

| Condition | Result |
| --- | --- |
| Socket not explicitly bound | Invalid operation/parameter before handshake |
| Mixed local and peer address families | Invalid parameter |
| Listen or accept requested | Invalid operation |
| Identical or contradictory cookie role | Connection rejection |
| HSv4 setup packet | `SRT_REJ_VERSION` |
| Incoherent pre-1.3 HSv5 identity | `SRT_REJ_ROGUE` |
| Mode, filter, congestion, or crypto mismatch | Compatible setup rejection |
| Overall deadline expires | Connection timeout |
| Socket closes during setup | Closed/cancelled connection result |

Rejected setup does not publish a connected runtime or application payload.

## Operational requirements

- Arrange for both UDP endpoints to be mutually reachable before starting.
- Preserve a stable local binding for the duration of the connection attempt.
- Configure sufficient handshake and peer-idle deadlines for the real network.
- Avoid application retry loops that create overlapping Rendezvous attempts on
  the same endpoint pair.
- Use a new socket after terminal failure; an existing broken socket is not an
  automatic reconnect object.
- Validate both start orders, both payload directions, and the intended
  encryption/FEC mode in deployment tests.

## Limitations

Release evidence is strongest for deterministic and single-host impairment
profiles. Physical NAT devices, asymmetric routing, address changes,
multi-hour outages, and independent two-node clock behavior remain
deployment-specific.

Robotweax does not claim arbitrary mixed-version Rendezvous behavior, HSv4
Rendezvous, mixed address families, Rendezvous Connection Groups, or automatic
path replacement.

See [Compatibility status](compatibility.md),
[Protocol edge cases](protocol-edge-cases.md), and
[Known limitations](limitations.md).
