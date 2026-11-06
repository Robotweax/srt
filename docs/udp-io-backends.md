# UDP I/O backends (draft contract)

**Status: draft proposal. Nothing in this document is implemented.** Every
option name below is provisional. **No throughput or CPU improvement is
claimed.** The draft exists so that the invariants, the backend boundary and
the qualification order can be reviewed before any code is written. Items
marked *open* need a decision. Items marked *assumption* rest on
operating-system documentation or platform knowledge and have not been tested
in this repository.

Robotweax SRT 0.3.2 moves every datagram with one system call. This draft
covers four optional additions, in the order they should be qualified: batched
system calls and UDP segmentation offload, then an `io_uring` backend on Linux
and a Registered I/O (RIO) backend on Windows. None of them changes the wire
format, the public C API or the default behavior.

## Current behavior

| | Linux | macOS and BSD | Windows |
| --- | --- | --- | --- |
| Send | `sendto` | `sendto` | `sendto` |
| Receive | `recvmsg` | `recvmsg` | `recvfrom` |
| Readiness | `epoll` | `kqueue` or `poll` | `WSAPoll` |

- **One native boundary.** `UdpSocket` in `src/udp.cpp` holds every native
  socket call. `DatagramChannel` in `src/compat/transport_runtime.cpp` is its
  only transport caller. The protocol core never touches a socket; see
  [Architecture](architecture.md).
- **Receive slice.** `DatagramChannel::run_receive_slice` reads up to 64
  datagrams, one at a time, into a single reused 1,500-byte buffer, dispatches
  each, then polls the channel's connections.
- **Send.** A connection encodes each datagram into a local 1,500-byte buffer
  and passes it to `ConnectionRuntime::submit_datagram`. That call sends
  synchronously under the channel send mutex and applies the completion
  effects before it returns. One poll submits at most 64 datagrams.
- **Backpressure.** `would_block` moves the datagram into a per-connection
  FIFO of at most 128 entries; see [UDP send backpressure](udp-backpressure.md).
- **Scheduling.** Each channel is pinned to one scheduler shard, and one
  readiness watcher thread per scheduler wakes it; see
  [Scheduler shards](scheduler-shards.md) and
  [Idle channel readiness](idle-readiness.md).
- **Adopted sockets.** `srt_bind_acquire()` and
  `robotweax_srt_connect_group_acquire()` take over UDP sockets that the
  application created.

## Goals and non-goals

Goals:

- Spend fewer system calls and kernel transitions per datagram where a
  measurement shows that this cost matters.
- Keep today's path as the default and as the fallback for every backend.
- Keep the change inside `UdpSocket`, `DatagramChannel` and the readiness
  watcher.

Non-goals:

- Kernel bypass (`AF_XDP`, DPDK) and zero-copy send.
- Kernel-side pacing or any change to pacing and congestion control.
- A thread per socket or per connection.
- A public socket option, a C ABI change or a wire change.
- Batching sends across connections. The batch unit is one connection.

## Invariants every backend must keep

Receive:

1. **Whole datagrams only.** A truncated datagram is consumed and skipped, as
   `Error::buffer_too_small` is today. It is never decoded as a shorter packet.
2. **Transient reports stay local.** A queued ICMP error or an interrupted
   call for one peer neither breaks the shared socket nor ends the slice.
3. **Slice budget.** A slice dispatches at most 64 datagrams before
   connections are polled. Unread input requests immediate work.
4. **Per-datagram routing.** Each datagram is dispatched with its own source
   endpoint and an ingress time. *Open:* a batch read yields one clock sample
   per batch unless kernel receive timestamps are requested; the effect on
   RTT and drift estimation must be measured before one sample is accepted.
5. **Bounded receive storage.** Receive memory is sized when the channel is
   created. Dispatch does not retain the receive buffer, which already holds
   because the buffer is reused for the next datagram.

Send:

6. **One completion point.** "Accepted by UDP" stays the moment at which
   wire-send statistics, pacing, FEC source admission, DATA-send timers and
   key packet counts advance, as [UDP send backpressure](udp-backpressure.md)
   defines it.
7. **Order and prefix.** DATA and nonperiodic controls of one connection reach
   UDP in submission order. A partly accepted batch commits exactly the
   accepted prefix; the rest stays queued with its retry accounting.
8. **Unchanged error classes.** `would_block` enters the existing FIFO, a
   short submission stays terminal, and permanent errors stay terminal.
9. **No early pacing release.** Only datagrams that are already due may share
   a batch.
10. **Fairness budgets.** The 64-item budgets per poll are unchanged; see
    [Shared-channel scheduling fairness](channel-fairness.md).

Structure and lifecycle:

11. **Bounded threads.** A backend may add at most one thread per scheduler
    generation, never one per socket or connection.
12. **Lifecycle contracts.** Close, final `srt_cleanup()`, fork and unload keep
    their present behavior. A request still outstanding at close must not
    complete into released state, and cleanup releases every ring, queue and
    registered buffer.
13. **Deterministic tests.** The existing send hook and the injected receive
    source keep working, and every backend has a deterministic fake.

## Backend boundary

The backend is chosen once, when a channel is created. A slice takes one branch
per slice, not per datagram, so the default path stays statically bound to
`UdpSocket` as it is today.

Two operations replace the per-datagram calls:

| Operation | Input | Result |
| --- | --- | --- |
| Receive batch | Up to 64 channel-owned slots | Count, plus size, source and status per slot |
| Submit batch | Due datagrams of one connection | Accepted prefix length and the first rejection |

One connection is the batch unit for three reasons. Its mutex is already held
while its datagrams are produced, so completion effects can run in order
without re-locking. The prefix rule of invariant 7 is defined per connection.
Segmentation offload requires one destination per call.

**Synchronous-batch rule.** Submit batch reports the result of every datagram
before it returns. Invariants 6 to 8 then hold with no in-flight state, and
buffers need to live only for the duration of the call. Batched system calls
and segmentation offload satisfy the rule by construction. `io_uring`
satisfies it if the assumption in its section holds. RIO cannot satisfy it,
which is why RIO is the largest change and comes last.

## Qualification order

| Step | Platform | Runtime model | Depends on |
| --- | --- | --- | --- |
| 0. Measurement gate | All | None | [Performance](performance.md) method |
| 1. `recvmmsg` / `sendmmsg` | Linux | Synchronous batch | Backend boundary |
| 2. Segmentation offload | Linux, then Windows | Synchronous batch | Step 1 |
| 3. `io_uring` | Linux | Synchronous batch | Step 1 |
| 4. Registered I/O | Windows | Asynchronous send completion | Step 1, in-flight state |

Step 0 records how much CPU the socket calls cost in the profiles that
[Scheduler shards](scheduler-shards.md) names under its packet-cost
experiments. If that share is not material, the later steps stop. Each later
step is compared on its own against the default path with the default shard
count.

## Batched system calls (Linux)

`recvmmsg` (Linux 2.6.33) and `sendmmsg` (Linux 3.0) keep the synchronous
model.

- **Receive.** One call fills up to 64 slots. Per-message truncation maps to
  invariant 1. The channel needs 64 slots of 1,500 bytes (96 KiB) instead of
  one stack buffer. *Open:* allocate the slots only for channels that have
  completed a full slice.
- **Send.** The return value is the accepted prefix. The first unsent datagram
  is submitted once more on its own to obtain its error class.
  *Assumption:* an error after at least one accepted message is not reported
  by the batch call itself.
- **Where batches form.** A paced live stream usually has one datagram due per
  poll, so it forms no send batch. Batches form on retransmission bursts, FEC
  output, File mode and catch-up after a late wake. The receive side of a busy
  shared listener is expected to gain more than the send side.

## Segmentation offload

Linux `UDP_SEGMENT` (Linux 4.18) can be passed per call as a control message,
so it changes no socket state. One call carries up to 64 equal-length
datagrams to one destination; the last may be shorter.

- Only a run of equal-length DATA for one peer qualifies. Controls and
  odd-sized datagrams are sent singly.
- *Assumption:* a segmented call is accepted or rejected as a whole, which
  would satisfy the prefix rule.
- The segments leave the host back to back. Invariant 9 limits a run to
  datagrams that are already due. *Open:* confirm with a capture that wire
  spacing does not differ from the default path in a way that affects the
  receiver.

Linux `UDP_GRO` (Linux 5.0) is a socket option. The kernel may then deliver
several datagrams in one buffer together with their segment size.

- The receive slot must hold a coalesced buffer, and invariant 1 applies to
  each segment after splitting.
- On an adopted socket this changes native socket state, as adoption already
  does for other options.
- *Open:* whether receive coalescing justifies the larger per-channel memory.

Windows lists `UDP_SEND_MSG_SIZE` and `UDP_RECV_MAX_COALESCED_SIZE` as UDP
socket options. *Assumption:* they behave like their Linux counterparts.
Supported Windows versions and failure behavior need a separate Windows
qualification.

## `io_uring` backend (Linux)

Kernel features this design uses, with the versions given by the Linux manual
pages:

| Feature | Since Linux |
| --- | --- |
| `eventfd` registration for completions | 5.2 |
| `sendmsg` / `recvmsg` operations, linked requests | 5.3 |
| Ring-mapped provided buffers | 5.19 |
| Multishot receive, single-issuer rings | 6.0 |
| Deferred task running | 6.1 |

The minimum is therefore Linux 6.0. *Open:* require 6.1 for deferred task
running.

- **Ring ownership.** One ring per scheduler shard, created when the first
  channel on that shard selects the backend. The shard thread is the only
  submitter. A send issued from any other thread keeps using `sendto` on the
  same descriptor.
- **Receive.** One multishot `recvmsg` per channel draws from a provided
  buffer ring and replaces the readiness arm and the receive loop. A slice
  consumes at most 64 completions and returns each buffer after dispatch.
  When the buffer ring runs empty the kernel ends the request with `-ENOBUFS`;
  the slice re-arms it. Datagrams wait in the socket receive buffer meanwhile,
  so nothing is lost that the default path would have kept.
- **Wake-up.** The ring's `eventfd` is watched by the existing readiness
  watcher, which keeps the scheduler model unchanged. The notification is only
  a hint, so a slice always inspects the completion queue. One wake hop per
  burst remains; the saving is in system calls per slice.
- **Send.** A connection's batch is submitted as linked nonblocking `sendmsg`
  requests, and the same call waits for the batch. A failure cancels the
  unstarted rest of the chain with `-ECANCELED`, which yields the accepted
  prefix exactly. `-EAGAIN` maps to `would_block`.
  *Assumption, to be proven by a test:* nonblocking UDP sends complete inside
  the submitting call, so the wait cannot stall the shard. If this does not
  hold, sends need the in-flight state described for RIO.
- **Completion queue.** It is sized for all receive buffers plus one send
  batch, so it cannot overflow by construction. *Open:* treat an overflow
  indication as a channel fault.
- **Availability.** Ring creation can fail on a capable kernel. Docker's
  default seccomp profile blocks the three `io_uring` system calls, and the
  `kernel.io_uring_disabled` sysctl can refuse creation with `EPERM`. Older
  kernels lack the required operations. The backend probes once per scheduler
  generation and never assumes availability from the kernel version.
- **Adopted sockets.** They work unchanged, because the backend needs no
  socket-creation flag.
- **Fork.** A child process must not use an inherited ring. *Open:* define the
  child's behavior so that it matches today's fork contract.
- **Sanitizers.** Lifecycle and TSan suites must run with the backend
  selected. Memory written by the kernel through the ring is not visible to
  MemorySanitizer.
- **Dependency.** *Open:* use `liburing`, which its repository describes as
  dual-licensed LGPL and MIT, or a small in-tree wrapper over the kernel
  header. A new dependency must be recorded in `THIRD_PARTY.md`.

## Registered I/O backend (Windows)

Microsoft's function reference documents the following:

- The function table is obtained through `WSAIoctl` with
  `SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER`.
- A request queue belongs to one socket and has fixed maximum numbers of
  outstanding receives and sends. Each direction names a completion queue; both
  may name the same one.
- Payload and remote address must lie in registered buffers. The address
  buffer holds a `SOCKADDR_INET`.
- Request and completion queues are not synchronized. Access from several
  threads must be serialized by the caller.
- Completion notification uses an event or an I/O completion port, chosen when
  the completion queue is created.
- The interface exists from Windows 8 and Windows Server 2012.

**Socket creation is the first thing to settle.** The Windows SDK header
defines `WSA_FLAG_REGISTERED_IO` for `WSASocket`, but the function reference
documents neither the flag nor the result of omitting it.
*Assumption, to be settled by a Windows test before any other RIO work:* a
request queue can be created only on a socket opened with that flag. If it
holds, library-created sockets must be opened through `WSASocketW` with the
flag instead of `socket()`, and an adopted socket can use RIO only when the
application opened it that way. In either case a failed request-queue creation
means that the socket stays on the default path. It is not an error.

- **Buffers.** Each channel registers one region for receive slots, send slots
  and address slots. Its size is fixed when the channel is created.
- **Serialization.** Sends on a request queue run under the existing channel
  send mutex. A completion queue belongs to one scheduler shard and is
  dequeued only there.
- **Receive.** The channel keeps a fixed number of receives posted. A slice
  dequeues at most 64 completions, dispatches them and posts the slots again.
- **Send completion is asynchronous.** `RIOSendEx` returns before the result
  is known, so the synchronous-batch rule cannot be met. A datagram occupies a
  registered slot until its completion is dequeued. The effects of invariant 6
  are applied at that point, pacing counts in-flight datagrams so that it does
  not over-submit, and close waits for or cancels outstanding requests.
- *Open:* alternatively treat a successful `RIOSendEx` return as "accepted by
  UDP" and handle a later failed completion as a lost datagram that ARQ
  repairs. This is much simpler, but it changes the meaning of invariant 6 and
  of the send statistics on Windows.
- **Queue limits.** The reference documents `WSAENOBUFS` for a full completion
  queue only. *Assumption:* reaching the outstanding-send limit can be mapped
  to `would_block`.
- **Notification.** The Windows readiness watcher uses `WSAPoll`, which cannot
  wait for RIO completions. *Open:* an event per shard completion queue or one
  completion port, served by one additional thread per scheduler generation.
- **Mixed calls.** The reference does not say whether `sendto` and `recvfrom`
  remain usable once a request queue exists. *Assumption needed:* if they are
  not, sends from other threads are routed through the request queue under the
  send mutex.
- **Platform scope.** Qualification is limited to Windows x64. The Windows
  ARM64 SDK is build- and link-only.

## Selection and failure behavior

- **Build.** Each backend sits behind a default-off CMake option, following
  the existing experimental options:
  `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_UDP_BATCHING`,
  `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_IO_URING` and
  `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_RIO`.
- **Runtime.** A process-level setting, `ROBOTWEAX_SRT_UDP_BACKEND`, is read
  when the scheduler is acquired and follows the rules of
  `ROBOTWEAX_SRT_SCHEDULER_SHARDS`. Its values are `default`, `batch`,
  `io_uring` and `rio`. An unset value selects today's path. An invalid value
  fails the operation that starts the scheduler and never selects another
  backend silently.
- *Open:* a valid backend that the host refuses, such as a blocked `io_uring`.
  Proposed: fail in the same way, because the operator asked for something
  the host cannot provide. Fall back per socket only for an adopted socket
  that cannot use RIO, and make that visible in diagnostics.
- **Compatibility.** The backend is local to one process. Peers, mixed
  deployments and the wire format are unaffected.

## Qualification

Each backend needs the following before it can be described as supported:

- **Contract tests.** One deterministic test per invariant against the
  backend's fake.
- **Native tests.** An oversized datagram, an ICMP unreachable report on a
  shared socket, backpressure with a small send buffer, close and cleanup with
  requests outstanding, the fairness budget on a shared listener, adopted
  sockets and multicast.
- **Existing suites.** Interoperability, encrypted rotation, AEAD, group, FEC,
  File mode, lifecycle and TSan runs stay clean with the backend selected.
- **Measurement.** Separate Release builds for baseline and candidate under
  the [Performance](performance.md) method. Record the kernel or Windows
  build, the container and seccomp state and the NIC offload settings.
  Delivered goodput and CPU cost are the primary outcomes. Offload also needs
  a wire-spacing capture.
- **Limits.** Supported platforms and remaining gaps are added to
  [Known limitations](limitations.md) when a backend is released.

No numerical improvement is claimed until an isolated comparison passes.

## Open decisions

1. One clock sample per receive batch, or kernel receive timestamps.
2. Lazy allocation of the 64 receive slots.
3. Receive coalescing: benefit against per-channel memory.
4. Minimum kernel for `io_uring`: 6.0 or 6.1.
5. `liburing` or an in-tree wrapper.
6. Fork behavior with an inherited ring.
7. Whether RIO requires `WSA_FLAG_REGISTERED_IO` at socket creation.
8. RIO send completion: in-flight state, or acceptance at `RIOSendEx` return.
9. RIO notification: event or completion port.
10. Behavior when a selected backend is unavailable on the host.
