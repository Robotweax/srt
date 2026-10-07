# Encryption and key rotation

Robotweax SRT 0.2.0 provides two encryption profiles:

| Profile | Availability | Security property |
| --- | --- | --- |
| AES-CTR | Default SRT 1.5.7-compatible build | Payload confidentiality |
| AES-GCM | Explicit `ENABLE_AEAD_API_PREVIEW` build | Payload confidentiality and authentication |

The GCM build option retains its historical preview name. In 0.2.0 it selects
the released, default-off Robotweax extension; it does not make GCM part of the
default SRT 1.5.7 public header or claim a complete SRT 1.6 API.

Both profiles use HSv5 key-material exchange, even/odd traffic keys, runtime
KMREQ/KMRSP rotation, and independent transmit and receive state. The complete
GCM wire and negotiation rules are defined in the
[AES-GCM contract](aes-gcm-contract.md).

## Building

The default build supports AES-CTR:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Enable the AES-GCM extension explicitly:

```sh
cmake -S . -B build-gcm \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_AEAD_API_PREVIEW=ON
cmake --build build-gcm --parallel
```

An extension-enabled CMake package propagates the matching compile definition
to consumers. The Linux pkg-config metadata includes it in `Cflags`. Library
and consumer must use the same profile; mixing default-profile headers with an
extension-enabled binary interface is unsupported.

OpenSSL 3 is the current cryptographic provider. Key schedules and provider
contexts are prepared when a key is installed and reused by packet processing.

## Socket configuration

Configure encryption before `srt_connect` or `srt_listen`. Accepted sockets
inherit the Listener's policy and complete negotiation during HSv5 setup.

| Option | Type | Public contract |
| --- | --- | --- |
| `SRTO_PASSPHRASE` | byte string | Empty disables encryption; a non-empty value must be 10–80 bytes and remains write-only |
| `SRTO_PBKEYLEN` | `int32_t` | `0`, `16`, `24`, or `32`; selects default/negotiated, AES-128, AES-192, or AES-256 key length |
| `SRTO_ENFORCEDENCRYPTION` | Boolean | Defaults to true; rejects a peer that cannot satisfy the configured encryption policy |
| `SRTO_KMREFRESHRATE` | nonnegative `int32_t` | Traffic-key refresh interval in consumed DATA sequence numbers, effectively capped at `2^30`; zero selects compatible default behavior |
| `SRTO_KMPREANNOUNCE` | nonnegative `int32_t` | Minimum number of consumed sequence positions before refresh at which the next key is announced; the runtime may announce earlier; zero selects the default minimum |
| `SRTO_KMSTATE` | read-only | Legacy: `SRTO_SNDKMSTATE` on a socket with `SRTO_SENDER` set, otherwise `SRTO_RCVKMSTATE` |
| `SRTO_SNDKMSTATE` | read-only | Transmit key-material state |
| `SRTO_RCVKMSTATE` | read-only | Receive key-material state |
| `SRTO_CRYPTOMODE` | conditional `int32_t` | Extension build only: `0` AUTO, `1` CTR, `2` GCM; after connecting, the active suite or `0` without encryption |

Invalid passphrase lengths are rejected before copying or deriving key
material. A value longer than 80 bytes is never truncated into a different
shared secret.

Some reference peers leave the handshake encryption field at `0` when
`SRTO_PBKEYLEN` is unset. This means no encryption length is advertised;
the key material carries the length. Callers and listeners accept this form.
A nonzero advertisement must match the corresponding key material.

For HSv5 Caller/Listener connections, an explicit caller `SRTO_PBKEYLEN` is
retained. A different nonzero length in the listener's INDUCTION response
fails establishment with `SRT_ESECFAIL` and `SRT_REJ_BADSECRET`; the caller
does not silently change its key length. An unset caller may adopt the
listener's advertised length, with AES-128 used when neither side selects one.
This is a deliberate stricter policy than the reference's role-dependent
conflict resolution, which can override an explicit length. See the
[reference option contract](https://github.com/Haivision/srt/blob/v1.5.7/docs/API/API-socket-options.md#srto_pbkeylen).

A listener's `SRTO_PBKEYLEN` selects an advertised/default length, rather than
an allowlist of permitted incoming lengths. In particular, a value set in the
listen callback does not change the length of an already supplied, valid
KMREQ. The final CONCLUSION response reports the accepted key length, and
`SRTO_PBKEYLEN` on the connected socket reports that length. This option does not
provide a per-key-length admission allowlist. Wrong passphrases and inconsistent KMREQ
advertisements still fail under enforced encryption. These Caller/Listener
rules do not change Rendezvous role or key-length selection.

### AES-CTR example

```c
const char passphrase[] = "replace-with-a-high-entropy-secret";
int key_length = 32;

srt_setsockopt(sock, 0, SRTO_PASSPHRASE,
               passphrase, (int)(sizeof(passphrase) - 1));
srt_setsockopt(sock, 0, SRTO_PBKEYLEN,
               &key_length, (int)sizeof(key_length));
```

Use the same passphrase policy at both endpoints. The selected key length and
key-material state can be queried after establishment.

### AES-GCM example

This code is available only to a consumer compiled for the extension profile:

```c
const char passphrase[] = "replace-with-a-high-entropy-secret";
int key_length = 32;
int crypto_mode = 2;

srt_setsockopt(sock, 0, SRTO_PASSPHRASE,
               passphrase, (int)(sizeof(passphrase) - 1));
srt_setsockopt(sock, 0, SRTO_PBKEYLEN,
               &key_length, (int)sizeof(key_length));
srt_setsockopt(sock, 0, SRTO_CRYPTOMODE,
               &crypto_mode, (int)sizeof(crypto_mode));
```

After connection, query `SRTO_CRYPTOMODE` and require effective value `2`.
This prevents an application from treating a CTR session as authenticated.
An unencrypted connection reports `0`, as does a connection whose peer holds no
usable key (see below), so the check also rejects both.

## Roles and transport modes

AES-CTR supports the normal Robotweax 0.2 encryption surface across
Caller/Listener and Rendezvous, IPv4 and IPv6, Live/Message and File/Stream,
supported FEC, and Broadcast/Backup groups.

AES-GCM supports these exact transport bundles:

| Bundle | Settings | Roles |
| --- | --- | --- |
| Live/Message | LiveCC, TSBPD on, Message API | Caller/Listener and Rendezvous over IPv4/IPv6 |
| File/Stream | FileCC, TSBPD off, Stream API | Caller/Listener and Rendezvous over IPv4/IPv6 |

Set `SRTO_TRANSTYPE=SRTT_FILE` to select the File/Stream bundle atomically.
Incoherent partial mutations fail with `SRT_EINVPARAM` rather than leaving a
partly changed connection profile.

The built-in FEC packet filter may be combined with GCM only in Live/Message
mode. GCM plus File/Stream plus FEC is invalid in either option-setting order.

For Rendezvous GCM, both peers must set explicit mode `2`. AUTO/GCM and
CTR/GCM pairs are rejected instead of being downgraded. In Caller/Listener,
an explicit GCM Caller may negotiate GCM through an AUTO Listener; all other
CTR/GCM mismatches fail.

## AES-CTR wire profile

AES-CTR encrypts only the DATA payload; the SRT DATA header remains clear.
The header's key bits select no key, the current even key, or the current odd
key.

- Stream keys are 128, 192, or 256 bits.
- A 128-bit salt and traffic key are generated with the provider's secure
  random generator.
- The key-encrypting key is
  `PBKDF2-HMAC-SHA1(passphrase, last_8_bytes(salt), 2048, key_length)`.
- Stream keys are wrapped with RFC 3394 AES Key Wrap.
- The 128-bit CTR block uses the network-order packet sequence in bytes 10–13,
  XORed with the first 14 salt bytes; bytes 14–15 begin at zero for the AES
  block counter.
- Initial KMREQ/KMRSP records follow HSREQ/HSRSP during HSv5 establishment.
- Runtime rotation uses User-Defined control subtypes 3 (KMREQ) and 4 (KMRSP).

AES-CTR does not cryptographically detect DATA-payload modification. Use GCM
or an application-level integrity mechanism where authentication is required.

## AES-GCM wire profile

GCM uses the same passphrase, salt, wrapping, KMREQ/KMRSP, selector, and
rotation framework, but accepts only the `(cipher=4, authentication=1)` suite.
The default CTR suite is `(cipher=2, authentication=0)`; mixed pairs are
rejected before key-state mutation.

For every DATA packet, GCM:

1. derives a 12-byte IV from the first 12 salt bytes and the network-order
   31-bit packet sequence;
2. authenticates the complete clear DATA header in network order, with only
   the retransmission bit normalized to zero;
3. encrypts the plaintext; and
4. appends a complete 16-byte tag.

The transmitted DATA body is `ciphertext || tag`. No shorter tag, alternate IV
size, host-order AAD, or surplus protected bytes is accepted.

The GCM tag is part of MSS and packet-filter resource accounting but not part
of application byte statistics. Validated unfiltered plaintext ceilings are
1440 bytes for IPv4/MSS 1500, 1420 bytes for IPv6/MSS 1500, and 1200 bytes for
IPv6/MSS 1280. FEC reserves its recovery header before the tag is subtracted.

## Key rotation

Transmit and receive directions own separate even/odd key pairs. The initiator
creates the initial key; the responder installs and acknowledges it during the
HSv5 exchange. Before sending DATA, each Robotweax endpoint then generates an
independent transmit key and salt and confirms them using the existing runtime
KMREQ/KMRSP exchange. The shared handshake material is retained only for
receiving from a legacy peer; it is not used as a bidirectional DATA key.
Later rotations proceed independently in each direction.

Connection establishment may therefore expose `SRTO_SNDKMSTATE=SECURING` while
the initial directional key is awaiting confirmation. Queued DATA is not
released before the matching response. Lost key messages are retried without
changing the pending key, and stale handshake acknowledgements cannot release
DATA. After the encrypted handshake succeeds, a key-control error cannot
downgrade this confirmation phase to plaintext. Wire formats, public options,
and the C ABI are unchanged; the extra key exchange can add startup latency.
This initial direction-separation behavior is included in the 0.2.3 source
tree, not in previously released 0.2.2 binaries. See the
[0.2.3 release notes](release-notes-0.2.3.md) for qualification status.

After initial encrypted key material has been confirmed, a local failure while
preparing the independent direction rejects connection setup, even with
`SRTO_ENFORCEDENCRYPTION=false`. It must not remove encryption and release
plaintext DATA.

Under optional encryption a listener that cannot use the caller's key
material answers with a four-byte KMRSP carrying the failure state instead
of dropping the extension: NOSECRET without a passphrase, BADSECRET with a
different one. It then reports `SRTO_RCVKMSTATE` NOSECRET or BADSECRET and
`SRTO_SNDKMSTATE` UNSECURED when no local passphrase exists. With a local
passphrase, its sender retains encryption and reports the failure state as
described below. The state occupies the first byte of the four-byte value
(`03 00 00 00` for NOSECRET), the layout the reference implementation
produces; both byte orders are accepted on input. An incompatible cipher or
invalid key-material format reports BADCRYPTOMODE, not BADSECRET. Repeated unusable runtime KMREQs retain the appropriate failure status;
a configured but unusable secret is not relabeled as missing.

With `SRTO_ENFORCEDENCRYPTION=false`, a receiver that cannot decrypt a DATA
packet, because it has no passphrase (NOSECRET) or could not unwrap the peer's
key (BADSECRET), counts the packet as undecryptable, acknowledges its sequence
number and discards the payload. Such data is never delivered, but the
connection stays up and the sender neither retransmits it nor stalls on flow
control, matching the reference implementation. With enforced encryption the
same undecipherable DATA is counted and dropped without acknowledging its
sequence or ending the established session. A later valid retransmission can
still fill the gap. Plaintext DATA on a secured CTR or GCM session follows the
same nonterminal drop policy. Under enforced encryption, encrypted DATA without
a usable local crypto session is also dropped without acknowledgement. An
unencrypted session without a local passphrase continues to accept plaintext.
AES-GCM packets that fail
authentication are never acknowledged, so a forged packet cannot suppress
genuine data.

In message mode, discarding one fragment rejects the entire message, including
fragments received later. Following complete messages remain readable. Missing
sequences still require normal loss recovery; rejection does not acknowledge
packets that have not arrived. In stream mode only the undecipherable packet
is discarded.

A side with a passphrase never falls back to plaintext, not even with
`SRTO_ENFORCEDENCRYPTION=false`. When the peer has no passphrase, a different
one, or answers the key material with a failure state, the connection is
still established, but this side keeps encrypting its DATA with its own key,
as the reference implementation does. The peer cannot decrypt these packets
and discards them; the application data is not exposed. This applies to
caller, listener and rendezvous alike. The side then reports
`SRTO_SNDKMSTATE` NOSECRET (BADSECRET for a different passphrase) and
`SRTO_RCVKMSTATE` UNSECURED if no peer key was offered, or the receive
failure state if its key material was rejected. `SRTO_KMSTATE` shows the send state on a socket
with `SRTO_SENDER` set and the receive state otherwise. `SRTO_CRYPTOMODE`
reports `0`. Such a sender does not send further KMREQs; key refresh still
switches to a freshly generated key on its own schedule, without
announcement. With AES-CTR it still delivers plaintext DATA from a peer
without a passphrase. With AES-GCM such unauthenticated DATA is counted as
undecryptable and never delivered.

Repeated runtime KMREQs after an optional key failure can receive the current
failure status at most once per 100 ms without disconnecting the peer or
resetting the local sending key. Such failures do not refresh peer liveness.
Once local-only encryption has been selected, delayed KMRSPs (including
failure duplicates and late confirmations) do not restart negotiation or
reset the key's sequence budget. Already secured sessions retain their
existing protection against unauthenticated key-control packets.

The receiver caches a key-encryption key only after the corresponding KMREQ
has been unwrapped and accepted. Invalid fresh-salt requests therefore cannot
evict the key used by normal rotations. In an established runtime, uncached
(fresh-salt) KMREQs are admitted through a token bucket of four PBKDF2
derivations per connection, refilled at one per 100 ms; rotations using the
validated cached salt bypass that budget. A request whose salt was recently
seen uses a separate lane limited to one derivation per 100 ms. A fixed-size
rotating Bloom filter remembers recent salts, including requests rejected by
the bucket: three 100 ms windows of 32,768 bits (12 KiB of filter storage,
allocated lazily on first fresh-salt admission) retain a candidate for
200–300 ms after its latest observation. Packet-count churn
does not evict entries within that interval; retries refresh it. An expired
candidate is classified against the current filter and can be first-seen again.
Filter lookup performs fixed work without further allocations. If initial
bookkeeping allocation fails, the request is not derived and can be retried.

This is a bounded, probabilistic mitigation, not a guarantee of timely key
exchange under attack. Hash collisions or filter saturation may classify a
new salt as a retry, so even a distinct-salt flood can compete for that lane.
Repeated attacker salts also compete for it, and either can delay a legitimate
request indefinitely. KMREQ has no independent authentication before its
wrapped keys are checked; authenticated control packets are needed for a
stronger security contract.


### Directional-key compatibility evidence

The interoperability mechanism is the existing runtime key exchange, not a
new cipher suite or a modified IV. Local qualification of this change uses
unmodified Haivision SRT v1.5.7
(`899348d8318eb9a3c5a5b6ec43c4a1114288773a`) and the pinned AEAD-preview revision
`d99d2e1a3b1a213b03c7dfbea5133898935fdeea`. Reference binaries are built locally
for testing and are not distributed with Robotweax artifacts.

To resolve the meaning of initial key cloning, the public v1.5.7 implementation
was inspected only at `haicrypt/hcrypt.c` (`HaiCrypt_Clone`) and
`haicrypt/hcrypt_ctx_tx.c` (`hcryptCtx_Tx_CloneKey`). At that immutable revision,
the cloning operation retains key and salt and prepares key announcements.
This is a version-scoped implementation observation, not a protocol requirement
to reuse a DATA key in both directions, and not a finding about an upstream
public-API vulnerability. Robotweax's direction-separation implementation and
regressions are independently authored; no upstream code or tests are copied.

By `refresh_rate - preannouncement`, the sender creates the inactive key and
sends a wrapped KMREQ; the adaptive window below may bring this forward. It
retries the same request until the matching KMRSP is
received, then changes the DATA selector at the refresh boundary. If the
KMRSP has not arrived by then, new DATA continues on the active key for at
most one further refresh period (twice the effective refresh interval in
total, never beyond the 31-bit IV space) and switches immediately after the
acknowledgement; only past that bound does new DATA pause. Runtime
retries wait `max(1.5 * SRTT, 10 ms)` after successful UDP submission once
an RTT observation is available; before that, the interval is 100 ms.
Both intervals are capped at half the configured peer-idle timeout, with a
10-ms minimum. The adaptive horizon is also capped at that peer-idle timeout.
Unauthenticated peer RTT estimates therefore cannot postpone retries for
minutes on a connection configured with a short local timeout. High-RTT
profiles should configure a suitable peer-idle timeout. Retries reuse the
same pending material and do not change the key sequence budget. A matching
KMRSP clears this retry clock so the next rotation can be announced immediately.
The runtime may announce earlier using the highest observed consumption rate
from samples at least 1 ms apart. Samples include sequence positions skipped by
TTL, too-late packet drop or group skips, and exclude retransmissions. The
configured preannouncement remains a minimum; its getter remains unchanged.
The adaptive window covers one SRTT, two retry intervals, four RTT-variation
estimates and 10 ms of scheduling margin. Before an RTT sample, it uses 100 ms
for the round trip and each retry. The observed peak is retained for the
connection, so a burst can keep later announcements early. Both the configured
and adaptive windows are capped below half the effective refresh interval.
This changes announcement timing only: key switching and the bounded overrun
at an unconfirmed refresh boundary retain their sequence budget.

Size this window for the highest expected rate of consumed sequence positions,
including positions skipped by TTL or too-late packet drop. For a steady rate
`R` positions/second, round-trip time `T` seconds, and up to `L` consecutive
lost KMREQ attempts or their responses, a starting estimate is
`preannouncement >= ceil(R * (T + L * retry_interval))`. Allow additional
margin for bursts, scheduling delay and RTT variation. Use the initial
100-ms retry interval when no RTT sample is available. This is a configuration
estimate, not a delivery guarantee. Startup before a usable rate sample,
sudden faster bursts, prolonged RTT increases and sustained control loss can
exhaust the adaptive window. The effective half-refresh cap
below still applies, so increase the refresh interval within its supported
key budget when the required window does not fit.

The deterministic runtime tests cover both CTR and GCM across two rotations
and sequence wrap. Besides steady rates, they exercise a fast startup before
the first usable rate sample, a burst before the next rate sample, and an RTT
increase from 4 ms to 50 ms, with zero or two lost requests or responses.
These transition profiles check exact payload delivery, the bounded overrun,
safe DATA pauses at its limit and recovery after key confirmation.
A further profile drops key requests or responses for two seconds during each
rotation and changes the peer RTT from 4 ms to 50 ms and back while DATA is
paused. It checks unchanged retry material, the current RTT retry floor, exact
payload recovery and the key budget across both rotations and sequence wrap.
These tests qualify controlled profiles, not network throughput, complete
network outages or arbitrary loss recovery.

For example, at 40,000 positions/second and 40-ms SRTT, the retry interval is
60 ms. A window for two lost attempts and the successful exchange needs at
least 6,400 positions before adding margin. The default 4,096-position window
covers about 102 ms at that rate. Set `SRTO_KMPREANNOUNCE` before connecting;
without confirmation, the active key continues for at most one further
refresh period before new DATA pauses safely.

The DATA IV is derived from the salt and the 31-bit sequence number, so the
key lifetime is measured in consumed sequence numbers, not in transmitted
packets. Sequence numbers that are skipped without a DATA packet being sent,
for example by too-late packet drop or an expired message TTL, count toward
the refresh as well.

`SRTO_KMREFRESHRATE` accepts values up to `2^31 - 1`, the maximum positive
`int32_t`; the same bound applies to the internal native configuration paths.
The smallest positive interval is 3; values 1 and 2 are rejected because no
positive pre-announcement fits below half of those intervals.
The effective refresh interval is capped at `2^30` sequence positions, and the
pre-announcement at half of that interval. Larger configured values are
accepted and reported unchanged, but rotate at the cap. Zero selects the
default, not an unlimited key lifetime.

The send buffer assigns a monotonic 64-bit position to each allocated packet.
Positions survive TTL/TLPKTDROP discards and 31-bit wire-sequence wrap. Before
selecting and encrypting new DATA, the runtime accounts for all skipped
positions and validates the prospective packet against the key budget. It
never derives this budget from a signed modular sequence distance or waits
until UDP submission to detect an exhausted budget.

If a gap carries the count past the refresh boundary before the successor key
is acknowledged, it consumes the bounded overrun; past that bound, new DATA
waits for KMREQ/KMRSP completion. If a jump would exhaust the remaining
31-bit IV space, the connection fails closed before encryption or submission.
Already protected retransmissions retain their original ciphertext and do not
consume another sequence position. Local UDP retries likewise reuse the
prepared ciphertext without reserving the position again.

Current receive keys and a bounded history of prior selector generations are
retained for delayed packets. Known delayed KMREQ duplicates can be answered
again without reinstalling old keys. A request containing only retired
selector keys is rejected while those generations remain in the bounded key
history (four per selector). A two-selector request with a retired key and a fresh companion may restore
that retired key only over replacements that have not received DATA. It cannot
erase a current or intervening retained generation already used for reception.
This is a bounded rollback guard: CTR reception does not authenticate the key
choice, and material older than both histories is still indistinguishable from
a fresh announcement. Unused provisional replacements can still be removed to
recover original retransmissions, while older legitimate generations remain
available. Known key bytes must retain the same
salt, selector, and cipher mode across both current receive slots and their
bounded histories; relabeled material is rejected without changing the session. Stale KMRSP messages cannot
acknowledge a newer request or roll the session back.
Ciphertext outside the bounded history fails closed. Like libsrt, this side
cannot tell a replayed announcement older than the whole history from a fresh
key: runtime KM messages are not authenticated, so such a replay installs the
old key until the peer's next rotation. Runtime KM messages are not bound to
an established sender identity; an actor with the passphrase and the ability
to send from the peer endpoint can still disrupt receive-key state.

For this residual attack, knowing the passphrase is not necessary if an attacker
can capture a valid KMREQ and inject it from the expected peer endpoint. Once
both bounded histories have forgotten the material, it may be reinstalled.
After packet sequence reuse, a captured GCM DATA packet under that same key and
salt can also have a valid authentication tag; GCM payload authentication is
not a connection-lifetime key-epoch replay defense.

Explicitly enabling `SRTO_ROBOTWEAX_SESSIONAUTH` on both peers prevents this
KMREQ rollback through authenticated monotonic counters, independently of the
bounded key histories. This remains default-off for ordinary Haivision wire
compatibility. Neither mandatory session authentication nor a new automatic
reconnection limit is silently imposed on compatible sessions. A lifetime
history, connection limit, or new authenticated wire epoch would need a
separate compatibility and resource contract.

## Retransmission and reliability

The first transmission replaces the send-buffer slot with its complete wire
payload and selector. Every retransmission reuses those bytes unchanged and
does not invoke the provider again or advance a key counter.

For GCM, the retransmission bit may change because it is excluded from AAD. No
other authenticated header field may change. This preserves the rule that one
`(key, IV)` pair creates only one cryptogram.

On receive, protected bytes are validated against the negotiated payload
budget before provider work. GCM plaintext remains provisional until the tag
passes. Only then can it enter reliability ordering, TSBPD, replay history, or
application delivery.

## FEC and encryption

FEC operates on wire payload rather than plaintext:

- with CTR, parity covers ciphertext;
- with GCM, parity covers `ciphertext || tag`;
- the receiver reconstructs the complete protected packet and authenticated
  header before decryption/authentication; and
- corrupt parity or a failed reconstructed tag publishes no plaintext.

Row, Column, and recursive Matrix filters are supported for encrypted
Live/Message traffic. Fragmented authenticated Message DATA is not emitted
when the recovery header cannot reproduce all authenticated boundary fields.

## Connection Groups

Set the encryption options on a Broadcast or Backup group before its first
connection. The group copies the configuration into each initial,
late-joining, or replacement member.

Every member negotiates its own mode and owns independent salts, traffic keys,
rotation, receive history, receiver identity, sequence context, and
ciphertext. Keys and protected payloads are never shared between paths.

Broadcast encrypts the logical message independently for each member. Backup
replay retains bounded logical plaintext and original message/timing metadata,
then encrypts it through the replacement member. If required replay history is
no longer available, failover fails closed rather than emitting an incomplete
or unauthenticated prefix.

## Error and security behavior

AES-CTR does not authenticate DATA or its sequence/key-selection metadata.
Receive-window and sequence-epoch checks bound accepted input, but cannot
distinguish forged in-window traffic from genuine reordered packets or prove
which retained key generation encrypted it. Applications requiring DATA
integrity must use and verify the negotiated AES-GCM profile described above.
AES-GCM authenticates DATA, not SRT control packets: a plausible DROPREQ can
still affect reliability state. These checks do not replace authentication of
the transport peer and path.

Controlled CTR/GCM tests cover one-position and 5,000-position peer skips
with an eight-packet receive buffer, including sequence wrap. A wrong source
endpoint, truncated payload or ambiguous distant range leaves the receive
floor and peer liveness unchanged. A valid range advances the floor once,
and subsequent protected DATA retains its exact payload. These are source,
shape and range checks; the accepted DROPREQ itself has no authentication tag.

- An incompatible cryptographic mode or transport bundle rejects
  establishment with the bad-crypto-mode state.
- A wrong passphrase reports the normal bad-secret result before protected
  DATA is accepted.
- Malformed, mixed, or unsupported key material is rejected without replacing
  installed keys or downgrading an already secured session.
- A missing/truncated GCM tag, AAD mismatch, or tag failure erases provisional
  output and drops the packet.
- Authentication failure does not refresh peer liveness, advance reliability,
  or advance receive-key sequence state. A later authentic retransmission may
  recover normally.
- Logs and public errors do not contain passphrases, derived keys, salts,
  plaintext, ciphertext, or authentication tags.

PBKDF2-HMAC-SHA1 with 2048 iterations is retained for SRT wire compatibility,
not as a password-storage recommendation. Use a high-entropy passphrase unique
to the intended trust domain and protect it as an application secret.

## Interoperability boundary

The default AES-CTR profile is qualified for AES-128/192/256 operation in both
implementation directions across Caller/Listener and Rendezvous, including
Live/Message and File/Stream rotation and recovery.

The released Robotweax-to-Robotweax GCM profile covers AES-128/192/256
primitives, IPv4/IPv6 Caller/Listener and Rendezvous, Live/Message,
File/Stream, FEC, Broadcast/Backup groups, rotation, and supported timing
profiles.

Positive GCM interoperability with the pinned capable reference profile is
limited to AES-128 Live/Message. Cross-implementation GCM File/Stream,
AES-192/256 transfer, encrypted mixed-implementation groups, and
reference-side Caller/Listener rotation are not claimed in 0.2.0.

Validate passphrase policy, mode selection, MSS, latency, loss recovery,
rotation intervals, and failover behavior under the intended deployment's
network and workload before production use.

See [Optional session authentication](session-authentication.md) for the default-off
`SRTO_ROBOTWEAX_SESSIONAUTH` option and its private wire contract.
