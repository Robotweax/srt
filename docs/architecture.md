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
hints coalesce into fixed flags; a hint published during dispatch requests another
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

An internal connection datagram inbox owns a fixed ring allocated at setup.
A shared storage budget charges the entire ring, including empty entries and
closed inboxes still retained by publishers or consumers. Credits return only
after ring destruction. The budget covers typed ring storage, including entry
metadata, and excludes allocator bookkeeping, inbox objects and consumer-owned
copies. Capacity multiplication is checked before reservation and allocation.

Each inbox has an immutable process-local incarnation and scheduler service
identity. Publishers and consumers must present the matching token; peer and
packet framing are validated before admission. DATA is limited to capacity
minus a configured control reserve. Structurally decoded control packets may
use that reserve, but share one FIFO with DATA; this is an admission policy,
not control authentication or priority reordering. Full admission rejects the
new datagram without displacing older entries.

Publication copies bytes and notifies the reserved service under the inbox
mutex. The scheduler never invokes its callback inline. Each accepted insertion
notifies, including during an active consumer turn; a bounded consumer rearms
when work remains. If notification fails, admission closes and queued work is
discarded rather than stranded. Close does not wait for a consumer and does not
revoke a previously popped copy; the consumer still needs the runtime close
barrier before effects. Inbox callbacks must avoid strong ownership cycles.
Queue highwater, DATA/control rejections, failed notifications and monotonic
queue delay are available internally. Public setup does not create these
inboxes or reserve their services, so UDP delivery remains on the existing
channel path.

An optional datagram dispatcher binds an already constructed runtime to a
reserved connection service and its owned inbox. The service context holds the
runtime weakly and pins it only for an executing callback. Each turn consumes
at most its configured budget of 1–16 datagrams, decodes handshake datagrams
through the replay handler and delivers other packets through the existing
runtime packet handler. Remaining work rearms the same service. Queue timing
uses a monotonic clock; an injected clock must support concurrent producer and
consumer calls and keep its context free of ownership cycles.

Runtime ingress admission snapshots reject local close and broken state;
packet and handshake handlers still enforce that barrier under their mutex.
Retiring the dispatcher closes admission and retires its service, but an
already dispatched callback can finish. Explicit dispatcher close additionally
uses the existing synchronous runtime close path. Callback quiescence remains
a separate query. A popped datagram cannot mutate a locally closed runtime.
Peer shutdown preserves the existing buffered-prefix and late-packet behavior.
Failed service notification also marks the target runtime broken.

Internal `finish_retirement(deadline)` retires admission and waits for the
binding's client callback to return before reclaiming the connection inbox ring.
Retirement and callback entry share a mutex: a service task already copied by a
worker cannot start a new client callback after retirement. This barrier tracks
callback activity independently of scheduler ownership; expiry of a weak
scheduler reference does not prove completion while its destructor joins workers.
All affinity workers reject the wait. Off-worker callers must hold no runtime,
route or inbox lock needed by the callback. A deadline timeout retains storage
and credits, and callers can retry. This operation does not close the runtime or
stop the scheduler; call explicit close first when late protocol mutation must
be prevented. Already dispatched work may finish against a live runtime.

After callback completion, ring destruction precedes budget credit return.
Concurrent drainers serialize reclamation and report actual completion. Captured
closed inbox handles remain safe and retain metadata, without retaining that
ring or returning its credits twice. The barrier also covers a paused setup-prefix
callback, even when the connection inbox has no in-flight entry. The old setup
ring and runtime identity can remain alive through captured setup handles and
are outside the connection ring budget. Callback completion does not promise
immediate service-slot reuse or destruction of scheduler task contexts; the
worker epilogue and generation rules still govern those resources. Route-wide
and process-wide cleanup, fault fanout and public activation remain separate
integration steps.

Internal channel `retire_connection(id)` removes the route incarnation and
retires dispatcher admission outside the route lock. It returns that dispatcher
as a drain receipt. An off-worker owner can retain the receipt across a deadline
timeout and retry `finish_retirement`; later socket-ID reuse cannot redirect
that receipt to the new route. Missing and direct routes return no dispatcher.
No runtime close is implicit. The legacy unregister wrapper attempts immediate
reclamation off-worker without waiting; it skips the drain on affinity workers.
If a callback is active, callers needing explicit credit-return proof must use
and retain the receipt. Dropping it leaves ordinary shared ownership responsible
for eventual destruction; captured closed inbox handles can still retain a ring
that was not explicitly reclaimed. No hidden retirement queue is allocated.

Fatal shared-channel failure closes established-route admission and channel
packet routing, then retires each remaining dispatcher and marks its runtime
broken outside the route lock. An intrusive cursor follows concurrent detach
without allocating a fanout snapshot; the first fatal failure owns this pass.
Routes remain owned until detached, providing the same drain receipts after a
fault. Concurrently detached routes belong to their detaching owner, rather than
a later fault pass. A paused protocol callback can delay runtime fault marking
under its existing mutex; this is not a preemption or wall-clock fanout bound.
Transient receive errors keep the current route admission behavior. A failed
channel requires replacement for new established connections. Listener/setup
cleanup, scheduled-channel stop, process cleanup/restart, and retained-receipt
ownership at public handle close are addressed incrementally below.

Owning compatibility close now detaches the route and transfers its retirement
receipt into `close_connection_runtime`. The close helper requests eventual
ring reclamation before establishing the runtime close barrier, even when the
runtime was already locally closed. An accepted executor close task owns the
receipt through its final protocol drain. A full/stopped executor or allocation
failure still takes the existing bounded synchronous protocol close fallback;
it cannot discard the callback's ring-reclamation obligation.

Each dispatcher binding has a bounded completion hook with the same context as
its client callback. Reclamation is requested once per dispatcher. If a callback
is active, its completion hook frees the closed ring only after protocol work
returns; a request racing the hook gets a further pass before quiescence is
published. If no callback is active, retirement performs the requested bounded
reclamation inline, including on a worker, without a callback wait or network
I/O. The quiescence barrier includes the hook. Captured closed inbox handles
remain safe, and dropping the external dispatcher/receipt cannot strand the
ring credit while the executing scheduler task owns the completion context.
No additional retirement queue or waiter thread is introduced.

Registry handle close and deferred-linger completion use this owning boundary,
as do established-setup failure close paths. Admission retirement remains
distinct from runtime close; `retire_and_reclaim()` alone permits already
dispatched work against a live runtime to finish. Service-slot reuse and task
context destruction still require the scheduler epilogue. The hook reclaims
only the connection ring, not captured original setup rings or arbitrary task
contexts. Listener/setup teardown, scheduled channel stop and full process
cleanup/restart qualification are continued by the internal boundary below.

Internal `DatagramChannel::shutdown(deadline)` is terminal for the whole shared
channel. It rejects affinity workers before changing state; concurrent or
reentrant calls report busy. It fences start, route/setup/listener registration
and channel dispatch, cancels the readiness watch/timer and waits for the active
scheduled channel task. The supplied deadline covers that task barrier (default
250 ms). Timeout retains the scheduler/context, routes and native socket for a
retry, with channel admission and scheduling still fenced. Captured dispatcher
handles retain their runtime admission until the retry reaches route teardown.
The deadline does not preempt runtime mutex acquisition, protocol network retry,
or inbox ready handlers.

Once the channel task is quiet, route/setup tables are swapped into local owning
batches without a fanout allocation. Readiness callbacks and runtime close run
outside route/lifecycle locks. Listener inbox publication is closed; setup
inboxes get an explicit terminal retirement fence. Ordinary setup `close()`
continues its existing promoted-handle forwarding contract. Terminal retirement
rejects publication even through promoted handles and rejects later promotion.
Established routes transfer their dispatchers to the owning close boundary;
active connection callbacks still finish asynchronous ring reclamation. The
native socket is retired under the send mutex after the runtime close attempts,
and later direct channel sends fail. Already captured original setup rings and
ready callback contexts retain their existing lifetimes.

A retired status proves channel task stop, route detachment and socket retirement;
it does not promise that every connection callback or higher-level listener/
setup operation has been joined. The shutdown boundary is internal and explicit.
It does not close a shared listener channel when just one accepted socket or the
listener handle closes. Process-wide cleanup ownership, listener/setup operation
joins and restart/generation qualification remain further integration gates.

Listener handshake and accepted-setup actor `stop()` now distinguishes terminal
result publication from terminal completion. An external stop waits until the
original terminal result-ready callback has returned and an active actor task
has left its client work. A terminal callback or that actor task reentering
`stop()` only requests close and returns, preserving self-close without waiting
on itself. Results remain observable through `wait()` as soon as terminal state
is published; result availability is not a callback join.

This strengthens the existing listener/setup stop calls used by owning handle
close and process cleanup. It adds no finite shutdown deadline and does not
provide an arbitrary affinity-worker wait API. Queued task-context epilogues,
callbacks installed after terminal publication and process-wide channel
retirement remain separate ownership concerns. The full cleanup/restart gate is
not completed by this actor barrier alone.

Final process cleanup now detaches bound-channel identities before registry
teardown and captures surviving bound/adopted channels in a retirement owner.
A second capture after socket-record teardown includes binds that were already
holding a record mutex at the first capture. Binding admission prepares the
batch node and geometrically grown channel slots before publishing a new
binding. Capture uses those slots without a teardown allocation, so old handles
can keep their channel object alive without keeping its native socket open.
The dormant binding registry is initialized among socket-registry teardown
dependencies, with the existing process-owned fork behavior.

After listener/setup record close and work-executor/scheduler stop, an external
final cleanup completes channel retirement before reopening lifecycle admission.
Normal individual socket close retains shared-channel behavior; nested cleanup
references do not retire the binding generation. New bindings cannot reuse old
binding identities after capture. Callback waits and native shutdown happen
outside the binding-registry mutex.

Internal retirement finish serializes concurrent/reentrant callers and retains
unfinished channel ownership on busy, task-barrier timeout or affinity-worker
rejection. A subsequent off-worker finish, including a later final cleanup,
retries those batches. There is no background retry thread. Reentrant calls into an existing cleanup
do not join its outer owner; the final worker transition is guarded separately
below. Pending batches consume
storage proportional to their still-owned binding generations, until retry;
there is no new process-wide channel admission cap. The finish deadline covers
channel-task barriers only, retaining the channel shutdown mutex/network/callback
limits. Connection callback ring completion and higher-level operation/resource
qualification remain separate gates; this is not public dispatcher activation.

The final runtime-cleanup transition rejects an affinity scheduler worker before
consuming the last startup reference, changing the cleanup gate or detaching any
registry/service ownership. The public wrapper reports `SRT_ERROR` / `SRT_EINVOP`;
an off-worker caller can then perform the preserved final cleanup. Nonfinal nested
reference release, zero-reference cleanup and reentry into an already active
cleanup keep their existing behavior. This prevents scheduler self-join without
creating a detached cleanup thread or pretending a rejected final teardown joined.

A lifecycle regression uses an actual dispatcher callback paused after pop on an
independently owned scheduler. Its final cleanup attempt is rejected. Off-worker
process cleanup closes the old runtime/channel while the paused copy keeps its
ring credits; a fresh same-port binding has a different channel/handle and cannot
reserve those credits early. Callback completion releases the old ring, allowing
the fresh dispatcher to reuse the budget, while stale old publishers remain
closed and the old copied packet cannot mutate either generation. An independently
owned scheduler must still be joined by its owner; this does not activate the
process dispatcher's service capacity or establish an API/wire total order.

Dispatcher FIFO covers its sealed setup prefix followed by its connection inbox.
It does not impose a total order on application operations or independently
invoked runtime handlers. The dispatcher does not poll the runtime or grant it a
separate send allowance; channel polling and send budgets remain on their
existing path. Internal route registration can select this dispatcher explicitly.
Public setup does not create or select it, so the default transport is unchanged.

The internal dispatcher factory can also claim a cold, unregistered setup inbox.
It seals that inbox without allocating or copying a second prefix ring. A worker
consumes the accepted setup prefix before its normal connection inbox; both
sources share the same per-turn limit. The prefix may be larger than the new
inbox or its DATA allowance. Full admission to the new inbox cannot displace
accepted setup packets. Prefix completion becomes observable only after the
last popped prefix copy finishes its protocol handler, and the worker releases
its old-ring reference outside metadata locks. An empty prefix still receives
an initial service notification so ownership can be released.

Captured setup publishers forward new datagrams to the same generation-bound
connection inbox, including after setup close. The forwarding reference is weak;
retirement or an expired dispatcher rejects publication rather than falling back
to synchronous runtime delivery. Factory failure leaves the original setup queue
available to its existing consumer. A sealed setup inbox retains its original
runtime identity while external publishers hold it. Until prefix completion,
the service also retains that existing setup inbox and its runtime pin; its
ordinary inbox alone continues to hold the runtime weakly.

Existing setup-ring storage remains bounded by its original capacity and is not
charged to the new connection-ring storage budget. That budget does not claim to
cover total process or transient setup memory. Claiming a prefix through the standalone factory requires the
caller to own an unregistered setup inbox; this factory is not an atomic channel
route promotion operation. A caller promoting a registered setup inbox instead
supplies a fresh dispatcher without a prefix to the optional channel route API.
Public setup continues using the existing synchronous promotion path.

Optional connection routes own a dispatcher bound to that exact runtime, peer
and channel. Registration rejects a previously used, retired or already claimed
dispatcher. Optional routes also reject runtime aliases that would mix direct
and queued delivery; ordinary direct registration checks for queued aliases only
when such routes exist. All route/replay-map allocation completes before setup
sealing. Under the route and setup-inbox locks, promotion claims the cold service,
seals the original ring, installs weak forwarding and publishes the route before
activation. Activation only publishes a service hint; it invokes no clock or
protocol callback inline. Failure unlinks the route and cached replay key and
restores the original setup queue and route. Failed activation retires the
service and marks its target broken after releasing both locks.

Channel ingress snapshots strong runtime/dispatcher handles under the route
lock and publishes owned datagram copies after unlocking. Established replay
admission checks peer, cookie and terminal state outside that lock; the worker
repeats the authoritative check before responding. Invalid replays retain the
existing setup/listener fallback. Valid replays rejected by full admission are
consumed without listener fallback. The decoded-envelope replay API queues the
base identity fields used by the established replay handler; actual wire ingress
queues the original datagram. Unregister unlinks the route and replay key, then
retires the dispatcher and releases owning handles outside the route lock. Old
setup publishers cannot retarget a reused socket identifier.

Channel polling skips a route while its sealed setup prefix has unfinished
protocol effects, retaining the bounded idle fallback. Dispatcher turns wake
channel polling after ingress effects or prefix completion. Polling continues
to use the existing shared channel send allowance. Ordering between queued new
ingress and runtime deadlines, complete cleanup quiescence and public transport
activation require separate qualification; the route API alone does not claim
exclusive shard ownership or a total application/wire order.

A dispatcher-bound inbox also fences publication through the target runtime
mutex, including publication through captured inbox handles. The bounded owned
copy and pure service notification complete before that mutex is released;
injected queue clocks are sampled outside it. Publication and channel timeout
checks therefore have a defined admission boundary. Generic inboxes keep their
existing admission path. A bound inbox permits only one popped copy at a time
and records it as in-flight until its protocol handler returns. Close discards
queued copies but retains this record; protocol completion clears it even after
close. This record is not callback quiescence, and bound inbox consumption belongs
to the dispatcher.

After the sealed setup prefix has completed, channel polling for an optional
route with queued or in-flight ingress grants peer-idle processing grace of at most its existing idle fallback interval
(default 2 ms), capped at one configured peer-idle timeout. The limit is measured
from the last real protocol activity, not from successive polls or publications.
New, rejected or ignored traffic does not move that limit; an empty inbox loses
the grace immediately. Once the fixed limit expires, timeout remains terminal
even if ingress is still pending. A stalled service therefore cannot postpone
peer timeout indefinitely. Only protocol handlers update peer activity.

This grace applies to peer idle alone. Its idle wait hint reflects the fixed
grace deadline instead of rearming a past peer deadline. Direct runtime polling
and default routes without an ingress context preserve their existing timeout
behavior.

Optional channel polling additionally captures one finite normal-ingress cohort
under the same runtime admission mutex: its cutoff is the inbox's admitted FIFO
count, and completion advances only after each protocol handler returns. New
publications and rejected frames cannot extend that cutoff. Generic inboxes do
not have these counters; a bound counter exhaustion closes admission and makes
the runtime terminal rather than wrapping into an old completion fence. Closing
an inbox discards entries without fabricating protocol completion.

Before an ordinary poll, an unfinished cohort gets at most the configured idle
interval, hard capped at 2 ms, from its first capture. The already fixed peer-idle
limit remains the outer terminal bound. Completion releases an ordinary poll
before any later cohort can be captured. Expiration releases polling until that
original cohort finishes; continued arrivals cannot restart its wait. Closing
or replacing the bound inbox also releases the fence. The wait consumes no
shared send allowance, and other routes continue their own channel poll visits.

This is bounded precedence over ordinary poll work, including receiver drops,
control timers, sender TTL/RTO and sends. It may add up to 2 ms of per-connection
poll/send/delivery latency when a worker is delayed. On expiry those decisions
may overtake queued effects, preserving progress under sustained ingress or a
stalled service. It is not an unconditional total wire/API order, nor a guarantee
that packet admission precedes a timer after an arbitrarily stalled handler.
Application calls and packet-triggered protocol work retain their existing mutex
semantics; direct runtime polling retains the noncoordinated default. Complete
scheduled poll/send ownership, callback/cleanup quiescence and public activation
still require separate qualification.

A pending sealed setup prefix also has a terminal peer-idle check. Channel
polling tries the runtime mutex without waiting; if available, it checks the
same fixed activity-based limit and capped processing grace. It performs no
ordinary protocol poll, receiver delivery or outgoing send before prefix effects
complete, and consumes none of the shared send allowance. On expiration or
runtime close it retires the dispatcher outside runtime and route locks. A
queued or popped prefix copy then cannot mutate the terminal runtime when the
service resumes, and old publishers cannot fall back to direct delivery.
Already accepted receive-buffer data retains the existing broken-runtime
semantics.

If another protocol/application operation owns the runtime mutex, the channel
keeps its existing bounded idle retry. The next available check uses the same
fixed deadline. This does not preempt a callback or establish a wall-clock bound
for a permanently held runtime mutex; callback/cleanup quiescence remains a
separate gate. Actual prefix handler activity can renew the ordinary peer timer,
while ignored frames and additional queue publication cannot.

Established handshake replay routing uses the immutable peer endpoint, peer
socket identifier and setup replay response. Reading this identity does not
acquire the runtime mutex. Registration snapshots it before taking the routing
mutex and stores the optional key with the route; removal uses that stored key.
The removed route's runtime remains pinned until after the routing mutex is
released, so final runtime/service-context destruction runs outside that lock.
On the default synchronous path, setup promotion seals the bounded datagram
inbox while publishing the established route. Setup consumers can no longer remove the sealed prefix.
Prefix processing runs after releasing the routing and inbox mutexes; a separate
promotion mutex serializes its completion. Established datagram delivery,
handshake replay and polling complete that prefix before protocol effects.
Once the prefix has completed, the route drops its temporary inbox reference.

A publisher which already captured the old setup inbox forwards to the original
runtime after prefix completion, including after setup-source cleanup closes
that inbox. It cannot append to the sealed queue or target a replacement route.
The old inbox pins its runtime until the last captured reference is released;
the runtime's close barrier still rejects protocol effects after local close.
Failed route registration leaves the setup queue available to its consumer.
This prefix barrier does not impose a total order on concurrent application and
wire operations. Shared-socket error propagation retains its separate
serialization path.

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
