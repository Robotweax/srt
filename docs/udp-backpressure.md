# UDP send backpressure

Established compatibility-runtime connections retain an encoded datagram when
nonblocking UDP returns `Error::would_block`. Previously, the first such result
broke the connection. The runtime now retries the same bytes without creating a
thread, changing the wire format, or renegotiating transport features.

## Submission and completion

An unblocked send still calls UDP directly, with no retry allocation or copy.
On backpressure, an immutable copy enters a per-connection FIFO. Further DATA
and FEC preparation pauses until that FIFO drains; incoming control responses
can still be queued. Each queued datagram is allocated only when needed and
released on completion or cancellation. The FIFO accepts at most 128 datagrams,
with at most 1,500 wire bytes per entry plus bookkeeping and allocator overhead.
Allocation failure or overflow fails the connection explicitly rather than
silently discarding required protocol messages or growing memory without bound.

Retries use the existing channel scheduler, at most once per millisecond while
UDP continues to return `would_block`. Repeated receive-driven polls before the
retry deadline do not issue another send. A poll submits at most 64 queued
entries before yielding. The interval is a scheduling target, not a wall-clock
delivery guarantee. Permanent UDP errors, short datagram submissions, and the
existing peer-idle timeout remain terminal.

Preparing a DATA packet reserves its send-buffer slot and, for encryption,
preserves its original protected payload. Wire-send statistics, pacing, FEC
source admission, DATA-send timers, and encryption key packet counts advance
only after UDP successfully accepts the datagram. Retrying never seals or
encrypts the same original again. FEC controls are consumed only after a
successful submission. KMREQ retry timing likewise starts on UDP success.

Sender TTL and TLPKTDROP checks run before retry submission. Queued DATA whose
slot was acknowledged or dropped is canceled, and an acknowledged/replaced
KMREQ is discarded. This prevents a delayed local retry from reviving expired
DATA or interfering with a later key rotation. Send-drain waits include queued
datagrams. Close cancels deferred output after its existing best-effort shutdown
attempt; it does not leave a transport worker running after close.

## Scope and validation

The change covers DATA, retransmissions, FEC, reliability controls, key material,
peer-error controls, and established-runtime handshake replay. Initial caller
and listener setup sends outside `ConnectionRuntime` are a separate path. This
is a recovery improvement for transient local UDP backpressure, not a claim of
higher network throughput or unlimited tolerance of sustained overload.

Run deterministic injection tests with:

```sh
build/robotweax_srt_tests backpressure
build/robotweax_srt_tests retains_packet
```

They cover repeated failures and exact byte preservation, pacing after a delayed
send, TTL/TLPKTDROP cancellation, ACK cancellation of a retransmission, FIFO and
drain behavior, close/permanent failure, the bounded overload failure, control
statistics, and AES-CTR/AES-GCM key rotation with FEC. The encrypted transfer
injects `would_block` before every successful datagram in both directions.

These tests complement the complete CTest suite, sanitizer runs, and black-box
interoperability tests against the pinned Haivision SRT 1.5.7 reference. Hook
injection reproduces the error deterministically; it does not claim to reproduce
every operating system's socket-buffer saturation behavior. Cross-platform
kernel-pressure and large-connection load tests remain separate validation.
