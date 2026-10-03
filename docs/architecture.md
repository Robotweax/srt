# Architecture

Robotweax SRT is organized around explicit protocol state, bounded storage,
and a stable C compatibility boundary. The architecture is intentionally
independent from the internal design of any other SRT implementation.

This page describes contracts that matter to integrators and contributors.
Private class names, worker counts, and data-structure layouts are not public
ABI and may change between pre-1.0 releases.

## Design principles

1. Decode untrusted input completely before mutating connection state.
2. Keep packet codecs allocation-free and independent from clocks and I/O.
3. Keep queues, rings, timers, retransmission state, and replay state bounded.
4. Represent protocol transitions as typed events and explicit actions.
5. Inject clocks, randomness, and network effects for deterministic tests.
6. Keep wire types and public C layouts separate from internal C++ types.
7. Fail closed when capacity, negotiation, or authentication invariants fail.
8. Avoid a permanent protocol thread per connection.

## Layer model

```text
Application
    │
Installed compatible and native C APIs
    │
Connection, lifecycle, epoll, groups, file helpers
    │
Handshake, reliability, congestion, timing, crypto, FEC
    │
Bounded packet codecs and buffers
    │
Nonblocking UDP transport
```

The compatible API translates public handles, options, errors, and callbacks
into typed internal operations. Protocol components do not depend on public
handle values or application object layouts.

Native C++ headers under `include/robotweax/srt/` are private source-tree
interfaces. They are intentionally not installed and may change as protocol
internals evolve.

## Module responsibilities

`codec`
: Decodes immutable packet views and serializes into caller-owned storage.
  Protected DATA is represented as one bounded `ciphertext || tag` body.

`handshake` / `rendezvous`
: Own HSv5 connection state. They consume typed datagrams and timers and emit
  bounded send, retry, connect, reject, or fail actions. UDP I/O is external.

`session` / `control` / `reliability`
: Maintain sequence space, receive windows, ACK/ACKACK, compressed multi-range
  NAK, loss aging, retransmission, DROPREQ, and recovery actions.

`send_buffer` / `receive_buffer`
: Preallocate packet rings for fragmentation, reassembly, stream notches, and
  retransmission. A protected sent packet is immutable across retransmission.

`timing` / `live`
: Model protocol deadlines, pacing, TSBPD timestamp mapping, drift correction,
  LiveCC estimation, and flow-window gates from caller-supplied monotonic time.

`file`
: Implements byte-stream extraction and FileCC without changing the bounded
  transport rings. File helpers adapt application descriptors to this stream.

`packet_filter` / `fec`
: Parse bounded filter configuration and generate or reconstruct Row, Column,
  and Matrix XOR recovery data. Recovery operates on protected bytes before
  decryption.

`crypto_provider`
: Supplies secure random generation, PBKDF2, key wrap/unwrap, secure erase,
  prepared AES-CTR, and prepared AES-GCM primitives through OpenSSL 3.

`crypto`
: Owns negotiated cipher mode, directional key slots, KMREQ/KMRSP state,
  acknowledged rotation, authentication, and replay rejection. Provider
  objects do not decide wire policy.

`socket_options`
: Validates configuration and derives handshake and runtime parameters. One
  payload-budget calculation accounts for MSS, address family, cipher tag,
  and packet-filter reservation.

`connection_group`
: Owns Broadcast or weighted Main/Backup membership, member-local transport
  state, ordering, late join, bounded replay, and failover policy.

`udp`
: Provides nonblocking IPv4/IPv6 datagrams, scope-aware endpoints, shared or
  acquired native sockets, and truncation-safe receive behavior. It contains
  no application policy.

`compat`
: Implements the v1.5.7-compatible C surface, process lifecycle, public handle
  registries, callbacks, epoll, connection orchestration, I/O, groups,
  statistics, logging, linger, and thread-local last errors.

`simulation`
: Runs the same protocol state machines over a virtual clock and deterministic
  datagram network.

## Packet and state safety

Decoders return either a complete validated representation or an error. A
malformed packet cannot partially apply handshake extensions, key material,
ACK state, loss ranges, FEC metadata, or application payload.

Network datagrams are length-delimited. A datagram larger than the configured
receive buffer is discarded as one unit; its truncated prefix is not passed to
the protocol decoder.

For authenticated DATA, decryption and tag verification complete before the
packet enters reliability state or becomes visible to the application.
Authentication failure erases provisional output and does not create ACK,
delivery, statistics, FEC-injection, or timing side effects associated with a
valid packet.

## Memory and resource model

Transport storage is sized from validated socket options before packet flow.
The hot path reuses:

- send and receive rings;
- loss and retransmission ranges;
- handshake and runtime inboxes;
- FEC group and duplicate state;
- connection-group replay storage;
- scheduler queues and timers; and
- prepared cryptographic key schedules.

Invalid sizes and arithmetic overflow fail during configuration. Exhausting a
bounded runtime queue or replay history produces an explicit error or terminal
connection outcome; Robotweax does not grow without limit or silently skip a
required protocol transition.

Heap allocation may occur during setup, option changes allowed by the public
contract, application-facing helper construction, and teardown. It is not a
normal per-packet requirement in the transport core.

## Scheduling and threading

Protocol work is scheduled by affinity so that one logical connection's state
machine is not mutated concurrently. A bounded process-wide execution model
services connection, listener, Rendezvous, UDP, callback-completion, and linger
work without assigning a permanent waiting thread to every socket.

Network I/O and application callbacks run outside protocol-state critical
sections. Readiness uses generation-based signalling so level- and edge-based
subscriptions do not lose a transition. Closing or final cleanup cancels
future work and wakes blocked public operations.

Applications must still synchronize their own data and must follow the
callback/lifecycle rules in the [Integration guide](integration.md).

The internal scheduler also supports explicitly configured persistent service
slots with a coalesced wake and one reserved deadline. These slots do not consume
ordinary job or timer capacity. Service callbacks run on the existing shard,
with round-robin service selection and alternating service/ordinary dispatch
when both are ready. Release closes service admission without blocking; an
already dispatched callback retains its context until it returns, so clients
must check their own generation and close barrier before effects. Final stop
cancels pending service wakes, joins active callbacks, and retires contexts
before concurrent stop callers return. The process scheduler currently reserves
zero service slots; the UDP transport does not use this internal facility.

Message send and receive separate one locked state attempt from the synchronous
wrapper's condition-variable waits. The attempt completes the existing buffer,
metadata, error and readiness mutations without retaining caller spans or
waiting for application capacity. The wrapper keeps the attempt and wait
predicate under the same runtime mutex, then notifies channel work after a
successful attempt has unlocked. A zero configured timeout still permits an
immediately available commit; nonblocking pressure and blocking timeout remain
distinct results. This separation does not introduce an asynchronous application
gateway or move application state onto a scheduler shard.
Application capacity and scheduler service capacity are separate: a reserved
service wake admits a future turn, not a completed send or receive. The current
message adapters do not wait for such a turn. They still contend on the runtime
mutex, and a nonblocking operation can wait for that serialization lock; only
its wait for application buffer capacity is disabled. Synthetic paused-service
checks freeze the worker before it takes a runtime lock. They do not qualify
transport affinity or callbacks that reenter a runtime whose lock is held.

An internal connection work binding can reserve one service at setup and keep
its shard, scheduler scope and slot generation fixed. Send and receive-release
hints coalesce into two flags; a hint published during dispatch requests another
turn. The binding retains no caller span or payload and allocates no storage per
notification. It weakly references the scheduler, while the service pins its
callback context until retirement. Client callbacks execute outside the hint
publication lock. Contexts must avoid strong ownership cycles with their binding
or runtime.

An explicitly supplied runtime binding receives hints after the runtime mutex
is released. The legacy channel wake is retained. A rejected hint cannot change
an already committed application result. Local close and runtime destruction
retire the binding without waiting; a dispatched callback can still finish and
must check the runtime close barrier before effects. Binding retirement is not
callback quiescence. Public connection setup supplies no binding, so this
internal facility does not change UDP routing or activate per-connection polling.

Established handshake replay routing uses the immutable peer endpoint, peer
socket identifier and setup replay response. Reading this identity does not
acquire the runtime mutex. Registration snapshots it before taking the routing
mutex and stores the optional key with the route; removal uses that stored key.
The removed route's runtime remains pinned until after the routing mutex is
released, so final runtime/service-context destruction runs outside that lock.
These rules apply to route registration and removal; setup-inbox draining and
shared-socket error propagation retain their separate serialization paths.

## Timing model

Protocol code consumes integer monotonic microseconds supplied by its owner.
It does not read a global clock during a state transition. Source-tree C++
callers using timed retransmission or loss-report retry gates must pass the
same monotonic microsecond clock to `next_data_packet`, `take_pending_report`
and `take_pending_reports`. The legacy zero defaults represent protocol time
zero for untimed fixtures; they cannot infer an application's current time.
These internal C++ interfaces are not part of the installed stable C ABI. ACK, NAK, keepalive,
retransmission, pacing, and TSBPD deadlines therefore use one explicit time
domain and can be reproduced under a virtual clock.

SRT source time and application media clocks are distinct. See
[Live timing](live-timing.md) for the public mapping contract.

## Encryption and retransmission

Cipher negotiation and key installation occur before DATA publication. Each
direction maintains independent key state. Connection-group members also keep
independent negotiated mode, key slots, receiver identity, rotation, replay
window, and ciphertext.

Retransmission reuses the immutable protected packet selected on first send.
It does not re-encrypt old plaintext under a newer key. Group replay retains
bounded plaintext only where a replacement member must create member-local
ciphertext with the original SRT metadata.

See [Encryption](encryption.md) and the
[AES-GCM extension contract](aes-gcm-contract.md).

## Error model

Expected validation and protocol failures use explicit error values. Exceptions
are not used for malformed packets or normal state transitions. The compatible
C API maps failures to its documented return sentinel and a thread-local SRT
error record.

An unsupported mode, unknown required capability, malformed extension,
resource overflow, authentication failure, or incoherent downgrade attempt
fails explicitly. No such condition may enable clear traffic, publish partial
state, or silently select a different protocol mode.

Detailed external contracts are listed in [Protocol edge cases](protocol-edge-cases.md),
[Public API compatibility](api-compatibility.md), and
[Known limitations](limitations.md).
