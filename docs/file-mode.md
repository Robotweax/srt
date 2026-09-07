# File/Stream mode and FileCC

Robotweax SRT File mode transports a continuous, reliable byte stream. It does
not preserve application write boundaries and does not schedule delivery with
TSBPD. FileCC controls the packet window and sending period used to carry the
stream.

File mode is suitable for file transfer and other ordered byte-stream
applications. Applications that need independent message boundaries or TSBPD
release times should use Live/Message mode instead.

## Selecting File/Stream mode

Set `SRTO_TRANSTYPE=SRTT_FILE` before bind or connection to select the File
mode option bundle:

- FileCC congestion control;
- Stream/buffer extraction;
- TSBPD disabled;
- TLPKTDROP disabled;
- periodic NAK reports disabled;
- the DATA retransmission flag kept clear;
- a maximum 1,456-byte DATA payload before other negotiated overhead; and
- linger enabled with a default of 180 seconds.

`SRTO_TRANSTYPE=SRTT_LIVE` restores the Live defaults. Two lower-level options
can override individual parts of the bundle before connection:

- `SRTO_MESSAGEAPI` selects Message or Stream extraction; and
- `SRTO_CONGESTION` selects `"live"` or `"file"` congestion control without
  changing the other transmission-type defaults.

Applications should normally select `SRTT_FILE` rather than constructing the
bundle option by option.

### Handshake negotiation

The HSv5 STREAM flag is derived from `SRTO_MESSAGEAPI`. A peer that cannot
accept the requested Message/Stream model rejects the handshake with
`SRT_REJ_MESSAGEAPI` (reason 12).

Non-default FileCC is carried in the optional HSv5 type-6 CONFIG extension.
Absence from an initiator request means LiveCC. A congestion-control mismatch
is rejected with `SRT_REJ_CONGESTION` (reason 13). A responder may accept an
explicit FileCC request without echoing the extension; if it does return an
explicit value, that value must match.

These rules apply to Caller/Listener and Rendezvous connections over IPv4 and
IPv6.

## Stream semantics

Consecutive DATA packets are exposed as one ordered byte stream:

- a send accepts as many bytes as fit in the preallocated send ring and may
  return a short positive count;
- a receive copies as many contiguous bytes as fit in the caller's buffer;
- one receive may cross packet and application-write boundaries;
- a receive may stop inside a packet and resume at the retained byte offset;
- out-of-order packets are retained, but delivery stops at the first sequence
  gap until recovery fills it;
- 31-bit packet-sequence rollover is handled with wrap-safe ordering; and
- peer shutdown produces a zero-byte end-of-stream result only after all
  already buffered bytes have been drained.

DATA still carries the normal packet-boundary and message-number fields on the
wire, but Stream extraction does not expose those boundaries to the
application.

### Partial I/O

Short positive send and receive results are normal. An application must loop
until it has transferred the desired byte count:

```text
while bytes remain:
    n = srt_send(...) or srt_recv(...)
    if n > 0: advance by n
    if n == 0 on receive: peer EOF
    if n < 0: inspect the SRT error
```

In nonblocking mode, a call that cannot currently make progress reports the
appropriate would-block condition. Blocking calls honor the configured
synchronization and timeout options. A zero-byte receive is EOF, not a short
read caused by packet framing.

## FileCC congestion control

FileCC maintains a congestion window and a packet sending period. Its initial
state is a 16-packet window, a 1 microsecond sending period, and a 10
millisecond rate-control interval.

The controller then uses acknowledgement and loss feedback to adapt:

- current Full ACK sequence progress grows the window during slow start;
- loss, timeout, or the maximum-window boundary ends slow start;
- receiver rate and RTT feedback determine the congestion-avoidance window;
- additive rate increase raises throughput when feedback permits it;
- loss ratios below 2 percent are tolerated before a congestion decrease;
- a congestion epoch increases the sending period by 3 percent; and
- `SRTO_MAXBW` supplies a lower bound on the sending period and can be changed
  at runtime.

A current Small or Full ACK replaces the peer receive-window value with the
ACK's Available Buffer Size. This includes a zero-window stop and a later
reopen. Stale ACKs and Lite ACKs do not change that state. Full ACKs
additionally carry receiver-rate and RTT metrics.

NAK ranges select missing packets for retransmission and provide loss feedback
to FileCC. Retransmissions retain priority over new DATA and remain eligible
while new sends are stopped by peer flow control.

## Retransmission timeout and recovery

The sender uses the SRT retransmission timeout equation:

```text
RTO = count * (SRTT + 4 * RTTVar + 2 * SYN) + SYN
SYN = 10 ms
```

The first successfully sent DATA packet arms the timer. A current cumulative
ACK resets the timeout count to one, a Full ACK supplies the peer SRTT and
RTTVar estimate, and an empty flight disarms the timer.

On expiry, FileCC's LATEREXMIT policy queues all sent, unacknowledged packets
when no NAK-selected retransmission is already pending. The queue is bounded
by the preallocated send ring and deduplicates packet identities. A NAK instead
schedules its specific missing ranges as soon as they are known.

File mode keeps the DATA retransmission flag clear for wire compatibility.
The receiver recognizes retransmission by packet identity and sequence state,
not by that flag. Recovery and cumulative ACK processing remain wrap-safe
across the 31-bit sequence boundary.

## Public file-helper API

The compatible API exports `srt_sendfile` and `srt_recvfile`. Both helpers:

- transfer through the continuous Stream API;
- use caller-selected block sizes independently of SRT packetization;
- advance the supplied file offset only by successfully transferred bytes;
- use one reusable application buffer per call; and
- remain blocking for the duration of the helper call, independently of
  `SRTO_SNDSYN` and `SRTO_RCVSYN`.

`srt_sendfile` accepts `size=-1` to send from the supplied offset to the end of
the file. Applications that require cancellation, event-loop integration, or
custom storage should implement a partial-I/O loop with the Stream send and
receive APIs instead of using the blocking helpers.

## EOF, shutdown, and linger

Receiving peer SHUTDOWN does not discard already buffered stream bytes. Reads
continue until that buffer is empty; the next receive returns zero to report
EOF.

`SRTO_LINGER` uses the public `struct linger` ABI. Its defaults are:

- Live mode: disabled, zero seconds; and
- File mode: enabled, 180 seconds.

With synchronous sending, close waits until cumulative acknowledgement drains
the send buffer or the configured linger deadline expires. With
`SRTO_SNDSYN=false`, close may return while the socket is `SRTS_CLOSING`.
Robotweax retains the runtime through a shared deferred-close worker, then
sends SHUTDOWN and releases the channel when drain or timeout completes.

## PEERERROR

If a receiving file transfer fails after it has begun, the receiver sends SRT
control type 8 with `SRT_EFILE` in the type-specific header field and exactly
one four-byte zero word (`00 00 00 00`) as its CIF. Empty, nonzero, or
differently sized CIFs are invalid; see the
[PEERERROR wire contract](protocol-edge-cases.md#peererror).

On receipt, Robotweax:

- wakes blocked Stream and file sends;
- reports `SRT_EPEERERR` from the next affected sending call;
- consumes that notification once; and
- leaves the connection established.

The application therefore owns recovery policy. If the failed sending call did
not accept its payload, retry or abort that payload explicitly. A malformed
type-8 control packet is ignored: it neither publishes PEERERROR nor refreshes
the peer-idle timer.

## Encryption

File mode supports the normal AES-CTR protection profiles with 16-, 24-, or
32-byte keys. Key rotation preserves reliable byte-stream ordering, and a
retransmission reuses the original immutable ciphertext and key selector.

The released AES-GCM extension is available only when Robotweax is built with
the historically named `ENABLE_AEAD_API_PREVIEW` option and both peers
negotiate the extension. For a 1,500-byte MSS, the maximum GCM application
payload is 1,440 bytes over IPv4 and 1,420 bytes over IPv6. The authentication
tag is transport overhead and is not counted as application payload.

GCM authentication completes before plaintext enters the receive stream. An
authentication failure cannot expose partial plaintext. Wrong passphrases are
rejected during handshake before DATA transfer.

GCM with File mode and FEC is an invalid combination. Robotweax-to-Robotweax
GCM File transfers are supported for Caller/Listener and Rendezvous; broader
cross-implementation GCM File interoperability is not claimed unless the peer
implements the same extension.

See [Encryption, key management, and AES-GCM](encryption.md) for negotiation,
wire format, rotation, and configuration details.

## Operational limits

- Stream mode provides ordered bytes, not message boundaries or TSBPD release
  timing.
- Positive short reads and writes are part of the API contract; callers must
  handle them correctly.
- `srt_sendfile` and `srt_recvfile` are blocking convenience helpers.
- FileCC adapts to transport feedback, but achievable throughput and recovery
  time still depend on RTT, bandwidth-delay product, buffer sizes, loss, and
  `SRTO_MAXBW`.
- PEERERROR is a one-shot application notification and does not automatically
  terminate the socket.
- File-mode linger can keep close work active until acknowledgement drain or
  the configured deadline.
- AES-GCM requires explicit build and peer negotiation and cannot be combined
  with FEC in File mode.
