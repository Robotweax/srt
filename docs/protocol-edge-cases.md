# Protocol edge-case policy

Robotweax SRT follows the selected
[`draft-sharabayko-srt-01`](https://datatracker.ietf.org/doc/html/draft-sharabayko-srt-01)
wire description and the public compatibility contract documented for release
0.2.5. This page collects security- or interoperability-sensitive rules that
are easy to miss when integrating SRT.

## UDP datagram boundaries

UDP preserves message boundaries. If a received datagram is larger than the
available buffer, Robotweax consumes and discards that complete datagram. Its
truncated prefix is never passed to packet decoding, handshake state, a shared
channel, or a connection runtime. A later valid datagram remains receivable.

This rule applies to IPv4 and IPv6 and protects every connection sharing the
same native UDP channel.

## Integrated HSv5 extensions

An integrated extension record consists of a four-byte type/word-count header
followed by exactly the advertised number of complete four-byte words.

Robotweax applies these receive rules transactionally:

- a truncated header or body rejects the complete handshake datagram;
- known HSREQ/HSRSP, KMREQ/KMRSP, or CONFIG records require their corresponding
  category bit;
- record order has no receive-side meaning;
- a well-framed unknown record is skipped for forward compatibility;
- duplicate known records are rejected rather than overwritten; and
- no accepted extension becomes visible until the complete proposal passes
  version, mode, crypto, congestion, filter, and group validation.

Robotweax emits the stable canonical order Handshake, Key Material, Stream ID,
Congestion, Packet Filter, and Group. Receivers must not depend on this order.

Category bits declare a record category; they do not guarantee that a usable
record is present. Whether a state requires HSREQ or HSRSP remains a handshake
state-machine rule.

## Fixed-width control fields

Packet sequence numbers are 31-bit values. Hostile words with bit 31 set are
rejected before construction of a wrap-safe sequence value. This applies to
ACK and DROPREQ sequence fields. Handshake ISNs accept the complete inclusive
range `0` through `0x7fffffff` and reject larger values.

The handshake flow window must be an interoperable signed value of at least two
packets. A connected sender keeps the committed peer flow window separately
from the local `SRTO_FC` receive-window configuration.

## ACK, NAK, and flow-window rules

- Lite, Small, and Full ACKs have distinct wire shapes and semantics. See
  [ACK and ACKACK semantics](acknowledgements.md).
- Only a current Small or Full ACK may replace the sender's peer receive
  window. A zero value closes the window to new DATA; a later current extended
  ACK reopens it.
- Stale ACKs cannot change the peer window. Lite ACKs retire cumulatively
  acknowledged DATA but carry no absolute available-buffer value.
- Requested retransmissions remain eligible while the new-DATA window is
  closed.
- New loss ranges retain their existing initial-report/reorder policy. Each
  reported range waits at least `SRTT + 4 * RTTVar` before another
  periodic NAK can include it. This prevents a half-RTT periodic timer from
  requesting the same retransmission while its first copy is still in flight.
  Independently discovered ranges remain immediately eligible for feedback.
  The periodic timer retains its 20 ms floor; it is not applied a second time
  as a per-range delay on low-RTT connections.
- A multi-range NAK is validated completely before it changes retransmission
  or congestion state.
- A NAK may request only DATA that has reached the wire. Future or queued but
  unsent packets cause the request to fail without partial mutation.
- Ranges older than retained sender history produce bounded DROPREQ responses;
  a range crossing the retention boundary is split with wrap-safe semantics.

## DROPREQ admission and grace storage

A DROPREQ identifies abandoned sequences, including a message whose queued
packets expired before reaching the wire. A group member can also use DROPREQ
to skip its unused part of the group sequence space. Such a range can exceed
the receiver's packet-storage capacity. Receiver admission therefore does not
require prior DATA for every abandoned sequence or impose a storage-window
limit on the range.

Robotweax applies an additional sequence-sanity policy: the inclusive range
contains at most `2^29` packets, and each endpoint must be within `2^29`
positions, forward or backward, of the current cumulative ACK boundary. This
quarter-cycle bound is an implementation policy, not a new wire field. Invalid
ranges leave receive slots, ACK/loss state and the TSBPD timestamp clock
unchanged. Locally generated group skips obey the same range-length limit and
fail before changing the sender sequence or queuing a control.

Live receivers can retain a bounded number of accepted ranges until their
playout grace expires. If this preallocated queue is full, an additional range
returns internal `would_block` before clock or reliability mutation; the
runtime ignores that control without terminating the connection or refreshing
peer activity. Duplicate ranges retain their original deadline and need no
additional queue entry. Grace expiry frees space for later controls or retries.

DROPREQ is not cryptographically authenticated by the SRT control format. A
plausible range from an on-path attacker or a spoofed peer endpoint remains
indistinguishable from a legitimate TTL or group notification. Sequence checks
limit implausible input; they do not prove the sender transmitted or abandoned
the claimed packets. See the [security model](../SECURITY.md#runtime-key-material).

## HSv5 version and downgrade policy

HSv5 discovery starts with a handshake packet whose numeric version is `4`.
That value alone is not agreement to use the legacy HSv4 protocol. A modern
Listener replies with version `5` and the SRT magic value; the following
CONCLUSION selects HSv5.

Robotweax 0.2 follows these rules:

- new production Caller, Listener, and Rendezvous connections use HSv5;
- a genuine legacy Caller response terminates with `SRT_ECONNREJ` and
  `SRT_REJ_VERSION`; no automatic downgrade is attempted;
- wire-ambiguous version-4/`UDT_DGRAM` discovery receives the normal stateless
  HSv5 response without allocating accepted state;
- a genuine legacy conclusion cannot invoke the listener callback, create an
  accepted socket, or signal accept readiness;
- structurally unambiguous legacy stream discovery cannot enter a positive
  session;
- every Rendezvous setup packet uses version 5; and
- an integrated SRT software identity older than 1.3.0 is incoherent HSv5 and
  is rejected as `SRT_REJ_ROGUE`.

`SRTO_MINVERSION` is a separate application policy. A coherent peer below the
configured minimum is rejected with `SRT_REJ_VERSION`. An optional omitted
HSRSP echo is not interpreted as software version zero.

Applications migrating from Robotweax 0.1 should read
[Migrating from 0.1 to 0.2](migration-0.2.md).

## Key-material and authenticated DATA

Key-material messages are validated for length, suite, mode, key selector,
rotation generation, and wrapped-key consistency before changing installed
key state. A malformed or stale message cannot:

- downgrade an encrypted connection;
- enable clear payload;
- acknowledge an unrelated rotation;
- replace a current key slot; or
- postpone a peer-idle deadline merely by being received.

For AES-GCM, DATA header and ciphertext authentication completes before any
plaintext, reliability state, FEC result, timing event, or application-visible
counter is published. A truncated tag, oversized protected body, replayed
packet, or tag mismatch fails closed.

## FEC ordering

FEC protects the encrypted DATA representation. On send, encryption precedes
recovery generation. On receive, FEC reconstructs the complete protected DATA
packet before decryption and authentication. This prevents reconstructed
plaintext from bypassing the negotiated cipher or authentication contract.

A recovered packet is still subject to normal sequence, duplicate, replay,
payload-size, and authentication validation.

For filter-requested ARQ, a stored DATA header, a discarded payload and a
settled peer-requested drop are all complete sequence decisions. They must not
be reintroduced as missing sequences by a later FEC loss report. When a peer
DROPREQ grace period expires, both loss lists release the now-dropped range;
true neighboring gaps remain eligible for NAK. While grace is still open,
missing original DATA can continue to be requested.

FEC loss-report scratch work scans the bounding span of clipped reported
ranges rather than every receive slot. Far-apart ranges can still span the
whole window, and transactional capacity/overlap validation remains in place.
No per-report allocation is added.

## PEERERROR

PEERERROR is an advisory control event, not a wire-level reconnect request.

- Robotweax emits control type 8 with the error code in the type-specific
  header field. Since 0.2.2, the CIF must be exactly one four-byte zero word
  (`00 00 00 00`), making the complete SRT packet 20 bytes including its
  16-byte control header. The error code is not carried in the CIF.
- Empty CIFs, nonzero CIF bytes, partial words, and surplus words are rejected.
- A malformed PEERERROR has no activity, wakeup, state, or deadline effect.
- A valid event wakes an affected blocked sender and the next send reports
  `SRT_EPEERERR` once.
- The individual connection is not automatically replaced.
- A broken or closing individual socket cannot be connected again; create a
  new socket for recovery.
- A Broadcast or Backup group may remain usable while another member is
  healthy, and the application may add a distinct replacement member.

The compatible API exposes the generic public error, not an additional accessor
for the peer's diagnostic word. The same zero-word CIF requirement applies to
[ACKACK](acknowledgements.md#ackack-control-information-field); see also the
[0.2.2 wire corrections](release-notes-0.2.2.md).

## Shutdown, EOF, and reconnect

Message and Stream shutdown preserve buffered-data ordering. Stream receive
returns zero only after already buffered bytes are drained and peer shutdown is
known. A local close follows the configured linger contract; final process
cleanup cancels pending linger work.

There is no automatic reconnect extension for an individual SRT socket.
Applications should create a new socket, reapply pre-connection options, and
establish a new handshake. Connection Groups may provide application-visible
path continuity, but each new member is still a distinct connection with its
own transport and crypto state. Group members share their timestamp origin
and receive TSBPD clock as described below.

## Group replacement and source time

A logical group message retains one absolute application source time. Every
member maps that value through the group's common timestamp origin. A late or
replacement member must use that origin and the established receive TSBPD
clock, rather than installing an independent handshake-derived playout
schedule. Raw timestamps from unrelated connections must not be substituted.

Backup promotion requires evidence that a candidate member acknowledges group
DATA and remains responsive for a stability interval. Local send-buffer
acceptance alone is not proof that a path is healthy.

Replay is bounded. If the required history has been evicted, recovery fails
explicitly rather than skipping a gap or publishing a discontinuous stream.

## Unsupported input

Robotweax distinguishes a well-framed unknown optional extension from malformed
or contradictory input. The former may be ignored for forward compatibility;
the latter is rejected. Unsupported required behavior, mode mixing, ambiguous
cipher negotiation, invalid address-family combinations, and resource overflow
are never silently approximated.

See [Compatibility status](compatibility.md) and
[Known limitations](limitations.md) for the release-wide boundary.

## Directional too-late packet drop

TLPKTDROP is independent in the two directions. The local receiver drops late
missing packets when local TLPKTDROP and receive TSBPD are enabled, even if the
peer disables its own receiver's TLPKTDROP or TSBPD. Sender dropping uses the
peer's advertised receive TLPKTDROP and negotiated send TSBPD; disabling local
receiver dropping does not disable this sender policy. `SRTO_SNDDROPDELAY=-1`
continues to disable sender dropping independently.

`SRTO_TLPKTDROP` getters return the configured local receive policy before and
after connection. The core `NegotiatedLiveOptions::too_late_packet_drop` field
represents receive dropping; `sender_too_late_packet_drop` represents send
dropping. Direct core configurations must set the latter explicitly when needed.
This follows the public [SRT socket-option contract](https://github.com/Haivision/srt/blob/master/docs/API/API-socket-options.md#SRTO_TLPKTDROP).
