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
| `SRTO_KMREFRESHRATE` | nonnegative `int32_t` | DATA-packet interval for traffic-key refresh; zero selects compatible default behavior |
| `SRTO_KMPREANNOUNCE` | nonnegative `int32_t` | Number of DATA packets before refresh at which the next key is announced; zero selects compatible default behavior |
| `SRTO_KMSTATE` | read-only | Combined key-material state |
| `SRTO_SNDKMSTATE` | read-only | Transmit key-material state |
| `SRTO_RCVKMSTATE` | read-only | Receive key-material state |
| `SRTO_CRYPTOMODE` | conditional `int32_t` | Extension build only: `0` AUTO, `1` CTR, `2` GCM |

Invalid passphrase lengths are rejected before copying or deriving key
material. A value longer than 80 bytes is never truncated into a different
shared secret.

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
plaintext DATA. Optional fallback remains available during the original
negotiation, before encrypted key material has been accepted.

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

At `refresh_rate - preannouncement`, the sender creates the inactive key and
sends a wrapped KMREQ. It retries the same request until the matching KMRSP is
received, then changes the DATA selector at the refresh boundary.

The refresh interval is capped at `2^31 - 1` DATA packets so that a traffic key
is rotated before a full 31-bit sequence/nonce cycle. This matches the maximum
positive `int32_t` value of `SRTO_KMREFRESHRATE`; the same upper bound is also
enforced by the internal native configuration paths. Zero retains its public
socket-option meaning of selecting the default, not an unlimited key lifetime.

If the next key has not been acknowledged at that boundary, new DATA pauses.
The sender never silently exceeds the configured lifetime or uses an
unacknowledged key. Already protected retransmissions remain eligible.

Current receive keys and a bounded history of prior selector generations are
retained for delayed packets. Known delayed KMREQ duplicates can be answered
again without reinstalling old keys. Stale KMRSP messages cannot acknowledge a
newer request or roll the session back. Input outside the bounded history fails
closed.

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
