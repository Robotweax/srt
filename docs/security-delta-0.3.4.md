# 0.3.4 security-sensitive delta disposition

This is a focused source-delta assessment for release preparation on 19 November
2026, not a new independent whole-repository security scan, a cryptographic
review or a protocol certification. The [2026-11-04 review](security-review-2026-11-04.md)
and the [0.3.3 delta disposition](security-delta-0.3.3.md) retain their original
scopes. Runtime changes inspected here are the delta from `v0.3.3` to
`f82da2a2ce12ccf86373b152fb0eeb8ee9865d53`. The assessment was made by reading
source; it executed no code. Final-source execution evidence belongs in the
release's separate package qualification record.

## Reviewed boundaries

- Group capacity: `GroupRegistry::add_member` is the single insertion point. It
  checks `ROBOTWEAX_SRT_MAX_GROUP_MEMBERS` and inserts the pending row under one
  lock, so pending and connected members share one count. Listener and caller
  enrolment failures remove the row again.
- Setup filtering: a caller ignores setup messages that do not fit its state or,
  once a cookie is bound, do not carry it. A rendezvous socket ignores packets
  whose cookie, socket ID or flow window differ from the remembered peer. The
  filter runs before peer socket ID, protocol, crypto policy, MSS and timestamp
  state are changed. A mismatch is now ignored instead of ending the attempt.
  Rejections received before a cookie is learnt remain unbound; only endpoint
  filtering protects that phase.
- File receive: the destination is opened once, without following a final
  symbolic link (POSIX) or reparse point (Windows), and its type is checked on
  the handle. Delayed truncation uses the same handle, so there is no
  check-to-open race on the final path component. Parent-directory links and
  hard links are outside this check by design and remain the application's
  responsibility. The Windows path was assessed by reading only.
- Receive buffers no longer zero-filled: for each such buffer the consumer reads
  only the length the producer reports, and decrypted output is used only after
  the provider reports success. The interval form of the buffer-overlap handling
  in `CryptoSession::open` erases the same bytes as the per-byte form for empty,
  disjoint, overlapping, adjacent and in-place layouts, and plaintext is still
  published only after tag verification.
- Sparse-traffic wait: the modelled deadlines are control, peer idle, sender
  retirement, sensor receive gap, receive delivery and tail probe, capped at
  10 ms. Pending key material or a due deadline keeps the 2 ms poll. The
  peer-idle check and key-rotation service can run up to 10 ms later than
  before. Key-use limits are enforced on the send path, not by this timer.
  Handshake deadlines were not traced in this assessment.
- Epoll wake: the wake flag is set under the record lock before the observer
  generation changes, and waiters sample the generation first, which closes the
  lost-wake window. Poll records are reference-counted and release only marks
  them, so a wake racing a release does not touch freed state.
- Hybrid freshness: only the time of the next query changed, and only to an
  earlier point. The freshness predicates are unchanged, so an expired
  confirmation cannot become fresh through this change.
- Null packet deletion: restore runs after decryption and, for AES-GCM, after
  authentication. All prefix and length checks complete before the first write,
  the largest expansion (one byte to 1,316 bytes) fits the 1,456-byte
  destination, and a failed restore drops the packet before reliability state
  sees it. With the default OFF policy neither encode nor restore is reached,
  and an OFF listener rejects a valid offer. FEC source DATA now refreshes peer
  liveness only after it validates, as non-FEC DATA already did. On cleartext
  and AES-CTR connections the restored content has no integrity protection, as
  for ordinary DATA.
- Default-path isolation of build-gated features: peer authentication sources,
  the `0x7f16` record decoder and UDP batching are compiled only with their
  build options. A default build treats `0x7f16` as an unknown record and sends
  the same handshake bytes as before.
- Peer authentication with its build option on: the listener compares the
  claimed key identifier with pinned hashes before any public-key work, allows
  one pending child with a deadline, and ends an attempt on the fourth
  unverifiable third message. Signatures are canonical low-S in both
  directions, peer points are validated, tags and identifiers are compared in
  constant time, and a connection is published only after both traffic-key
  directions confirm. The identity scalar, shared secret, key schedule and
  passphrase copies are erased on completion, failure and close. A REQUIRED
  listener's child cannot be turned OFF by the listen callback. This is a
  statement about the inspected code, not about the soundness of the design,
  which remains unreviewed.
- UDP batching with its build options on: a receive batch holds at most 64
  datagrams, a truncated datagram is not dispatched, and the peer address is
  taken per datagram. An encrypted original is counted against its key at
  staging and not again at completion, and key and IV are fixed per sequence at
  seal time; no route to key overuse or IV reuse was found.

No reportable runtime security defect in the default build was established by
this focused assessment. This statement is limited to the inspected boundaries;
it does not extend the supported security profile of any experimental feature.

## Observations for disposition

These were found by reading and are recorded for an owner decision before the
source freeze. None is fixed in this revision.

1. Peer authentication, option on: listener admission is serialised, and the
   serialisation predates this delta. A caller that passes the cookie exchange,
   names a pinned key identifier, which is public, and never sends its third
   message holds the listener's single pending slot until the connection
   timeout (3 seconds by default) or the retry limit. Other admissions on that
   listener wait meanwhile. The draft contract does not list this.
2. Peer authentication options in a default build: setting the policy, key or
   trusted-key option on a listening or connected socket returns
   `SRT_ECONNSOCK` because the state check runs first. The draft contract says
   every such option returns `SRT_EINVOP` in a build without the feature; a
   fresh socket does return `SRT_EINVOP`.
3. Peer authentication, option on: two expected-tag stack values are not erased
   on the mismatch return; one of them depends on the passphrase.
4. UDP batching, option on: the receive batch does not clamp the kernel's
   reported length to the slot size before use and does not zero the peer
   address storage, unlike the single-datagram path. Both rely on the kernel
   contract and no triggering input was identified.
5. Group enrolment: if a member is added and the group generation then differs
   from the listener mirror's generation, the removal uses the mirror's
   generation and may leave the pending row. Whether this state is reachable
   was not established.

## Not inspected

Tests, OpenSSL and libsrt internals, the BCrypt provider body, handshake
deadline handling under the sparse-traffic wait, the interaction of null packet
deletion with the session-authentication transcript, and peer-authentication
code outside the changed regions. Haivision compatibility of the setup filter
rests on the interoperability runs recorded for its pull request, not on this
reading.

## Retained limits

- The peer-authentication cryptographic design has had no independent review.
- Long-horizon compatible-mode KMREQ replay remains tracked and documented in
  [encryption](encryption.md) and the [security policy](../SECURITY.md).
- Experimental ACP1, hybrid control, multicast trust and revocation limits are
  unchanged from 0.3.3 and remain in [limitations](limitations.md).
