# Jumbo MSS (draft contract)

**Status: draft proposal. Nothing in this document is implemented.** Every
option name and limit below is provisional. **No throughput or CPU improvement
is claimed.** The draft exists so that the compatibility boundary, the storage
rules and the failure behavior can be reviewed before any code is written.
Items marked *open* need a decision. Items marked *assumption* rest on
operating-system documentation or platform knowledge and have not been tested
in this repository.

Robotweax SRT 0.3.2 accepts `SRTO_MSS` from 76 through 1,500 bytes; see
[Known limitations](limitations.md). This draft describes what it takes to
accept larger values on networks that carry jumbo frames end to end. The wire
format does not change, and behavior at 1,500 bytes and below stays exactly as
it is.

## Compatibility boundary

The handshake already carries the MSS as a 32-bit field, and both peers use the
lower advertised value. Handshake decoding checks only the lower bound of 76.
Jumbo MSS therefore needs no new record, no negotiation and no extension
policy.

| Local MSS | Peer advertises | Effective MSS | Result |
| --- | --- | --- | --- |
| 1,500 or less | Any value | 1,500 or less | Unchanged bytes and storage |
| Above 1,500 | 1,500 or less | 1,500 or less | Unchanged bytes; larger local reservation |
| Above 1,500 | Above 1,500 | The lower value | Jumbo DATA in both directions |

- A stock peer with default settings advertises 1,500 and is served as today.
- Interoperability above 1,500 is **not claimed** for any other
  implementation. The pinned reference release limits Live payloads to 1,456
  bytes, and its caller was observed in source review to reject a handshake
  response that carries an MSS above 1,500. An interoperability run must
  record this before the statement is published.
- A peer that advertises more than 1,500 is taken at its word. There is no
  separate capability check.

## Goals and non-goals

Goals:

- Fewer packets per byte on controlled networks: LAN, data centre and private
  WAN links with a known MTU.
- A loud failure when the path cannot carry the configured size.
- No change in wire bytes, memory reservation or option results for a socket
  whose MSS is 1,500 or less.

Non-goals:

- Path MTU discovery, including packetization-layer discovery.
- Reliance on IP fragmentation in any form.
- Larger control packets. Handshake, key material, ACK and NAK keep their
  present size bounds.
- A change to the default MSS or the default Live payload size.

## What depends on 1,500 bytes today

| Area | Today | Where |
| --- | --- | --- |
| Option range | Upper bound equals the 1,500 default | `src/socket_options.cpp`, `src/compat/socket_options.cpp` |
| DATA payload bound | Compile-time `maximum_data_payload_size = 1456` | `include/robotweax/srt/packet.hpp` |
| Payload pool slot | Slot size is that constant | `include/robotweax/srt/payload_pool.hpp` |
| Send and receive buffers | Reject payloads above that constant | `src/send_buffer.cpp`, `src/receive_buffer.cpp` |
| Payload budget | Clamped to that constant | `make_crypto_payload_budget` |
| FEC working storage | Fixed arrays of that constant | `include/robotweax/srt/fec.hpp` |
| Session authentication | Fixed buffers of that constant or 1,500 | `src/session_authentication.cpp` |
| NAK capacity | Loss ranges per report derive from that constant | `include/robotweax/srt/session.hpp` |
| Datagram storage | 1,500-byte stack buffers and `DatagramEnvelope::maximum_size` | `src/compat/transport_runtime.*`, `src/compat/connection.cpp` |
| Backpressure FIFO | 128 entries of `DatagramEnvelope::maximum_size` | `src/compat/transport_runtime.hpp` |
| Retransmission budget | Bucket capacity floor of 1,500 bytes | `src/compat/retransmission_budget.hpp` |
| Live API | `SRT_LIVE_MAX_PLSIZE = 1456` bounds `SRTO_PAYLOADSIZE` and Live sends | `include/srt/srt.h`, `src/compat/message_io.cpp` |

The estimator for the receive rate already uses the measured average payload
size and needs no change.

## Invariants

1. **Unchanged at 1,500 and below.** A socket configured with an MSS of 1,500
   or less produces the same wire bytes, reserves the same memory and returns
   the same option values and errors as 0.3.2.
2. **No wire change.** The MSS field keeps its meaning and the lower value
   wins.
3. **Control packets stay small.** Handshake, key material, ACK, NAK, drop
   requests and keepalives keep today's size bounds. NAK capacity stays at the
   value derived from 1,456 bytes. Setup and recovery signalling therefore
   never depend on the jumbo path.
4. **Storage is sized once.** `SRTO_MSS` is a pre-bind option. Every buffer is
   sized from the configured MSS when the socket or channel is created. Nothing
   is reallocated after the handshake.
5. **No per-packet allocation.** Acquiring and releasing payload slots stays
   allocation-free, as [Bounded payload storage](payload-storage.md) requires.
6. **Truncation rule.** A datagram larger than the channel's receive storage
   is discarded as a whole and never decoded as a shorter packet.
7. **Shared listener.** The receive storage of a shared UDP channel follows
   the listener's MSS. Accepted sockets inherit it and cannot raise it.
8. **Bounded maximum.** The accepted range has a fixed upper limit that is a
   documented constant, not the 65,535-byte IP limit by default.

## Maximum value

*Open:* the upper limit. Proposed: **9,000 bytes**, the common jumbo Ethernet
MTU. A higher limit raises every per-packet reservation for sockets that ask
for it and has no known deployment that needs it.

## Storage sizing

The compile-time payload constant becomes a per-socket value derived from the
configured MSS. The constant keeps its present value as the default and as the
bound for control payloads.

- **Payload pool.** The slot size is the maximum wire payload for the
  configured MSS. Buffer capacities are held in packets, and the byte values
  of `SRTO_SNDBUF` and `SRTO_RCVBUF` are realigned to `MSS - 28` as today. The
  reservation therefore grows in proportion to the MSS at an unchanged packet
  count: about 1.46 MB per 1,000 packets today and about 8.96 MB per 1,000
  packets at an IPv4 MSS of 9,000.
- **Datagram storage.** Stack buffers of 1,500 bytes become channel- or
  connection-owned storage of the configured size. A jumbo buffer does not
  belong on a scheduler thread's stack.
- **Backpressure FIFO.** Entries are allocated only when needed, at the size
  of the queued datagram. The 128-entry bound is unchanged, so the worst case
  per connection grows from about 188 KiB to about 1.1 MiB at MSS 9,000.
- **Effective MSS below the configured MSS.** The larger reservation stays in
  place for the life of the socket. *Open:* accept this, or size after the
  handshake at the cost of invariant 4.
- **UDP socket buffers.** `SRTO_UDP_SNDBUF` and `SRTO_UDP_RCVBUF` are already
  clamped to at least the MSS. At a fixed byte size they hold fewer datagrams,
  which the option documentation must say.
- **Retransmission budget.** The bucket capacity floor becomes the configured
  MSS. With a floor of 1,500 bytes and a low limit, a jumbo retransmission
  would exceed the capacity and could never be sent.

## Scope by feature

The first step keeps the change narrow. Combinations outside it fail when the
option is set, with `SRT_EINVPARAM`, as other unsupported combinations do.

| Feature | Step 1 | Later |
| --- | --- | --- |
| File/Stream mode, plaintext | Supported | |
| AES-CTR and AES-GCM | Supported | |
| Live/Message payload above 1,456 | Not available; ceiling stays 1,456 | Step 2 |
| Packet-filter FEC with MSS above 1,500 | Rejected | *Open* |
| Session authentication and ACP1 with MSS above 1,500 | Rejected | *Open* |
| Connection groups with MSS above 1,500 | Rejected | *Open* |
| Multicast dispatch groups with MSS above 1,500 | Rejected | *Open* |

- **File/Stream mode** gains without an API change, because its payload per
  packet is the MSS minus 44 bytes over IPv4 or 64 bytes over IPv6.
- **Live/Message sockets** may be configured with a jumbo MSS in step 1, but
  their payload ceiling stays at 1,456 bytes. They gain nothing until step 2.
- **Groups** are deferred because members can run over paths with different
  MTUs. *Open:* how a group derives one message ceiling from its members.

## Live payloads above 1,456 bytes (step 2)

`SRT_LIVE_MAX_PLSIZE` stays 1,456 in the public header, because applications
compile against it.

- `SRTO_PAYLOADSIZE` accepts values up to the MSS-derived ceiling when the
  configured MSS is above 1,500. Its default stays 1,316.
- A Live send is checked against the connection's effective ceiling instead of
  the constant.
- Payload size is not negotiated. A receiving application that reads with a
  1,456-byte buffer gets `SRT_ELARGEMSG` for a larger message, as it does
  today for any message that exceeds its buffer.
- *Open:* treat a jumbo MSS on the receiving side as consent to larger
  messages, which needs no wire change, or advertise a Live payload ceiling in
  a private handshake record. Proposed: the first, with clear documentation.

As an example of the gain, an IPv4 MSS of 9,000 leaves 8,956 payload bytes, or
8,940 with an AES-GCM tag. Either holds 47 MPEG-TS packets (8,836 bytes) where
the default Live payload holds 7.

## Path MTU and failure behavior

The library sets no don't-fragment option and performs no path MTU discovery;
the operating system's defaults apply. Handshake packets are small. Without
further measures a connection across a path that cannot carry the configured
MSS can therefore establish and then lose every full-size DATA packet, or have
it fragmented, which multiplies the effect of packet loss.

- **Don't fragment.** When the configured MSS is above 1,500, library-created
  sockets request that outgoing datagrams are never fragmented. On Linux this
  is `IP_MTU_DISCOVER` with `IP_PMTUDISC_DO`, under which the kernel rejects
  datagrams larger than the known path MTU with `EMSGSIZE`. Windows documents
  `IP_DONTFRAGMENT` for UDP. *Assumption:* macOS and IPv6 offer equivalent
  options on every supported target.
- **Loud failure.** `EMSGSIZE` is not in the set of transient send errors, so
  a rejected DATA send is terminal for the connection and reports the system
  error. This is the intended outcome. It needs a test and a documented error.
- **Silent loss remains possible.** A smaller MTU beyond the first hop is only
  known to the sender if an ICMP report arrives. If none does, DATA is lost
  without a local error and the connection ends through the existing peer-idle
  or delivery rules. *Open:* an optional setup-time probe that pads a
  handshake message to the effective MSS between two peers that both advertise
  more than 1,500. It would change handshake bytes for that case only and
  needs its own contract.
- **Adopted sockets.** *Open:* apply the don't-fragment option to a socket the
  application handed in, or leave its native state alone and document the
  application's responsibility.
- **Sockets at 1,500 and below.** Their fragmentation behavior is not changed.

## Relation to other drafts and features

- **Batched UDP I/O.** A draft for batched system calls and segmentation
  offload assumes 1,500-byte receive slots. If both are adopted, slot sizes
  follow the channel's configured MSS and the two share one sizing rule.
  *Assumption:* one segmented send is limited to about 64 KiB in total, so it
  carries about seven 9,000-byte datagrams.
- **Statistics.** Byte counters are unaffected. Packet counters fall for the
  same byte rate, which matters to anything that alarms on packet rates.
- **Integration guides.** The FFmpeg, GStreamer, VLC and OBS guides assume the
  default Live payload and are unaffected unless an application opts in.

## Selection

- **Build.** A default-off CMake option,
  `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_JUMBO_MSS`. When it is off, the accepted
  range is 76 through 1,500 exactly as in 0.3.2.
- **Runtime.** No new option. `SRTO_MSS` accepts the wider range, and its
  existing pre-bind rule applies.
- **Getters.** After connection `SRTO_MSS` returns the effective value, as
  today.

## Qualification

- **Unchanged-default proof.** Byte-identical captures and identical memory
  reservation for MSS 1,500 and below, with the option built in. The existing
  interoperability suites pass unchanged.
- **Deterministic tests.** Option range and rejected combinations, the
  negotiation table above, storage sizing, the truncation rule at one byte
  over the channel size, the retransmission budget at a jumbo size and the
  control-packet bounds of invariant 3.
- **Native tests.** Loopback transfers at several jumbo sizes in File/Stream
  mode with plaintext, CTR and GCM. A constrained path with a 1,500-byte hop
  between two jumbo endpoints qualifies the failure behavior, with and without
  ICMP reports.
- **Reference behavior.** A recorded run of a jumbo-configured Robotweax peer
  against the pinned reference release in both roles, to confirm that default
  reference settings yield an effective MSS of 1,500.
- **Measurement.** Goodput and CPU cost against MSS 1,500 on a jumbo-capable
  link under the [Performance](performance.md) method. Record NIC MTU, offload
  settings and kernel or Windows build.
- **Documentation on release.** [Known limitations](limitations.md) and
  [Socket options](socket-options.md) carry the new range, the supported
  combinations and the memory arithmetic.

No numerical improvement is claimed until an isolated comparison passes.

## Open decisions

1. The upper limit; proposed 9,000 bytes.
2. Keep the larger reservation when the effective MSS is lower.
3. FEC with a jumbo MSS.
4. Session authentication and ACP1 with a jumbo MSS.
5. Groups and multicast dispatch groups with a jumbo MSS.
6. Live payload consent: jumbo MSS alone, or an advertised ceiling.
7. A setup-time path probe.
8. The don't-fragment option on adopted sockets.
