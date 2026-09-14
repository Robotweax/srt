# Two-phase plain sender poll candidate

Lab [#43](https://github.com/Robotweax/srt_network_lab/pull/43), reviewed at
`350a3ca680756bbe8175eafe0cb76c6805eb1e25` and merged as
`75d79d6b06dc4ebc667875d148bc7a4421266786`, attributes 621/4106 and 542/4185
sender CPU samples to `ConnectionRuntime::mutex_` unlock work for Robotweax →
Robotweax and Robotweax → Haivision. Most are in `poll()` and `queue_message()`.
These are on-CPU samples, not lock wait times or a predicted speedup.

This candidate shortens the connection critical section around plain DATA
output. It is not performance-qualified. Baseline A remains
`8e1bdebed836cb7b732db852f51ef6a7b212e925`; the current main revision
`762328b51be1f764bd881ddd7e52140a30add8b5` has the same product source as A.
The lab handoff must pin the exact candidate commit separately.

## State and ownership contract

The optimized path requires `crypto_ == nullptr` and no active FEC encoder.
Crypto, key rotation and FEC retain their existing locked per-packet path.
Control output, receive processing and scheduler policy are unchanged.

For each plain DATA packet, while holding `mutex_`:

1. Check the existing peer flow window, retransmission priority and pacer.
2. Reserve the packet through `next_paced_data_packet()`, which already updates
   flight state and the pacer, as it does in A.
3. Encode the complete datagram into an owned 1500-byte stack array. Copy the
   payload length and retransmission flag needed by the successful-send commit.

The runtime then releases `mutex_`, sends only that owned encoded span using a
strong channel reference and copied endpoint, and reacquires `mutex_`. On success
it commits statistics and send/retransmission timers with the same pre-send
protocol timestamp used by A. On failure it marks the connection broken with
the original system error. Earlier successes remain counted; the failed packet
is not counted or retried by later polls. Every exit clears the output-in-progress
flag and notifies protocol waiters under the connection mutex.

Application `queue_message()`, `queue_stream()` and `queue_group_message()` may
append while output is in progress. They hold `mutex_` and cannot remove a
reserved send-buffer slot. Encoded output reads no session state or borrowed
payload while unlocked. Channel lifetime spans output and commit, including
when the application drops its last external channel reference.

ACK/NAK processing, another poll, handshake replay, receiver operations that can
emit control, dynamic options, sequence skipping, close and broken/error
transitions wait on `data_output_finished_`. Waiting releases `mutex_`, so it
cannot prevent the sender from reacquiring the mutex for its commit. There is
no additional lock-order edge. Statistics and send-state snapshots also wait,
so they cannot report a partially committed send. Simple state/crypto getters
and `writable()` may linearize before the send completes. Existing blocking
application waits release the mutex as before.

## Pacing and bounds

The current pacer rebases each next-send deadline on the actual selection time
and carries no accumulated burst credit. Preparing several packets at different
clock times and then emitting them together would change that behavior.
Consequently this first candidate owns **one datagram at a time**. The next
packet is selected only after output and commit finish, with a fresh pacer
query. It adds no sleeps, spinning, future credit or pacing-rate changes.

The original limit of **64 DATA packets per poll** remains shared by all
unlocked sends. Control/drop-request limits are unchanged. New work enqueued
during output may use the remaining poll budget; the final readiness/deadline
is computed from the current protected state. No heap allocation is added to
the send path. The runtime gains one condition variable and a flag; wire storage
remains one 1500-byte local array. No public API, option or exported ABI changes.

The tradeoff is explicit: producers can progress during UDP output, but every
plain packet adds an unlock/relock pair and waiting protocol threads use a
condition variable. It may improve contention or regress CPU cost. Correctness
tests cannot answer that performance question.

## Validation and promotion

`robotweax_srt_sender_poll_tests` exercises producer progress during blocked
output, channel lifetime, owned payload/order, competing poll and ACK exclusion,
close ordering, successful-prefix accounting after a send failure, waiter
release, flow-window behavior, retransmission priority, the 64-packet limit
across repeated unlocked sends, and prevention of preparation-time burst credit.
Existing runtime tests retain exact pacing deadlines and cover crypto, FEC,
file/stream mode, receiver drop behavior and lifecycle interactions. The new
small suite is also part of the Linux ThreadSanitizer CI job.

Before any product promotion, use a fixed plain A/B/B/A comparison in the original
four-vCPU, 6-GiB ARM64 lab VM. Use fresh A/B builds with the same clean harness,
public peer and toolchain; pin every source and executable hash. Keep profiler,
scheduler, packet limits, protocol options and VM settings fixed. Compare both
R→R and R→Haivision with equal useful bytes, real endpoint wait statuses,
payload integrity, sender/receiver CPU per GiB, context switches, packet counters
and UDP error deltas. Record all cases, including failures. No adaptive repeats
or selecting only favorable runs.

The previous acceptance thresholds remain useful for a first bounded screen:
both profiles must retain throughput ≥95% and sender/total CPU ≤105% of A, with
at least one profile improving throughput or total CPU by ≥5%. Stable Haivision
controls are required. Passing this screen nominates a follow-up; it does not
establish multi-stream scalability, WAN behavior or general performance leadership.
