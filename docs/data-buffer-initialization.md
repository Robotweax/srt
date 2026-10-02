# DATA send-buffer initialization

`ConnectionRuntime::send_data` uses an uninitialized 1,500-byte local buffer
instead of clearing the entire buffer before encoding. This removes redundant
work from each DATA submission without changing wire bytes, pacing, scheduling
or crypto state. Other datagram buffers remain unchanged.

## Written-prefix proof

Only `datagram.first(datagram_size)` can reach `submit_datagram`:

- Clear DATA: `encode_packet` writes all four header words and copies every
  payload byte. Its returned `bytes_written` is exactly header plus payload.
- CTR original DATA: the same complete encoding happens before in-place
  encryption. Encryption does not add bytes. Failure exits without submission.
- Retransmitted protected DATA: encoding copies the already preserved wire
  payload, including the authentication tag where applicable. It is not
  encrypted again.
- GCM original DATA: the encoder writes the complete header used as AAD;
  successful sealing writes the entire ciphertext and authentication tag.
  `prepare_protected_payload` bounds both spans; the submitted length is header
  plus those spans. Encoding, sealing, key-selection or preservation failure
  returns before submission.

Normal FEC DATA uses this path; filter-control encoding uses its separate
unchanged buffer. Local backpressure queues copy the exact submitted span;
retries never access the unused local buffer tail. Successful provider operations
must fulfill their existing complete-output contract. Initial zeroes cannot
substitute for valid ciphertext or authentication tags.

## Evidence and limits

In the baseline Apple ARM64 Release object, `send_data` contains a `bzero`
relocation for this clear. Inspect matching optimized objects for each platform
before extrapolating its cost. The candidate removes this particular clear;
other required initialization and failure erasure remain.

Regression verification must cover clear/CTR/GCM DATA, key rotation,
retransmission, FEC, groups and transient-send retries. ASan/UBSan detect bounds
and undefined operations but do not establish absence of uninitialized reads;
the written-prefix review is required. An MSan-capable Linux build can provide
additional coverage with a fully instrumented dependency stack.

No end-to-end throughput increase is claimed from removing one clear. Measure
this change separately at the default shard count with the same host, traffic
and source-linked helper, retaining every run. Encrypted relay capacity remains
a separate Linux qualification task.
