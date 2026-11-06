# Peer authentication (draft contract)

**Status: draft contract with private protocol components; no socket support.**
Every identifier below is provisional and unregistered. **The cryptographic
design is unreviewed.** Independent cryptographic review is required before
any support or security claim. Development stays behind the default-off
[build option](#build-option). Items marked *open* need a decision. Items
marked *assumption* could not be verified in this repository.

The current private implementation provides the record codec, transcript hashing,
two-stage key schedule, secret cleanup and a complete three-message proof
exchange using pinned P-256 identities. The OpenSSL provider implements P-256
key generation/import, public-point validation, ECDH and ECDSA; Mbed TLS and
BCrypt currently return `Error::unsupported` for those operations. Private
keys stay in provider-owned handles with no raw private-key export.

The native runtime can now import a completed exchange into a fresh
`CryptoSession`: key 4 authenticates KM and FEC using the peer-authentication
label, and keys 5/6 wrap the caller/listener transmit material without further
PBKDF2 work. Both directions generate fresh AES-GCM material and wait for KM
confirmation. Legacy handshake proofs, optional-encryption fallback and shared
group schedules are rejected for imported sessions. The runtime erases its
passphrase copy on import; the exchange owner still owns its cached flights
and must destroy the exchange on completion or failure.

Socket options, admission callbacks and transport-handshake integration are
still absent. Enabling the build option does not
authenticate socket connections or change their wire bytes. In particular,
`Exchange::ready_for_key_exchange()` is not permission to publish a caller or
release DATA: the caller still needs the listener's passphrase proof through
an authenticated runtime key control. The socket layer must supply cookie and
admission checks, normal transport-field validation, timeout/cancellation and
application publication. The contract below describes the intended complete
feature, not current socket behavior.

Build and exercise the private components with:

```sh
cmake -S . -B build-peerauth -DCMAKE_BUILD_TYPE=Debug \
  -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_PEERAUTH=ON \
  -DENABLE_AEAD_API_PREVIEW=ON
cmake --build build-peerauth --target robotweax_srt_tests --parallel
build-peerauth/robotweax_srt_tests --include peer_auth_
./tools/python tests/peer_authentication_vectors.py
```

The vector generator independently computes synthetic transcript and key-schedule
fixtures using Python's standard library; these are not ECDH/signature vectors
or evidence of cryptographic review. Separate P-256 tests use the public vectors
from RFC 6979 section A.2.5 and RFC 5903 section 8.1. Exchange tests cover
mutual authentication, pin rotation, transcript tampering, replay, wrong
passphrases, bounded invalid signatures, byte-identical retries and provider
failures. Runtime handoff tests cover all AES key lengths, directional KM
confirmation, fresh salts without PBKDF2, FEC replay rejection and failed
initialization. SHA-256 is implemented for each backend; platform qualification still
applies. The protocol components use bounded
storage and are not compiled into builds with the option off.

**Scope of version 1: it authenticates the handshake and key establishment,
not the runtime.** AES-GCM, which version 1 requires, authenticates source
DATA. Runtime controls such as ACK, NAK, DROPREQ, KEEPALIVE and SHUTDOWN stay
forgeable by anyone who can inject packets, so a connection between two
authenticated peers can still be disturbed or closed by a third party. Closing
that gap by composing with ACP1 is the
[first follow-up](#first-follow-up-authenticated-runtime-controls).

Peer authentication adds two properties that
[session authentication](session-authentication.md) and
[ACP1](experimental-acp1.md) state they lack: each peer proves possession of
its own private key, and traffic keys are protected by an ephemeral key
agreement so that a later leak of long-term secrets does not expose recorded
sessions. RIST Main Profile offers per-peer identity and forward secrecy
through DTLS; this format is independently designed for the SRT handshake and
is not interoperable with RIST.

## Goals and non-goals

Goals, against an attacker who controls the network and may hold the shared
passphrase:

- **Per-peer identity.** Each side verifies a signature made with a private key
  whose public key the application pinned. Holding the passphrase is no longer
  enough to impersonate a peer, and holding a pinned identity key without the
  passphrase is not enough either.
- **No passphrase guessing by strangers.** Nothing an endpoint sends depends
  on the passphrase until the other side has proven a pinned identity. The
  [residual exposure](#what-each-proof-establishes) is limited to holders of a
  stolen identity key.
- **Forward secrecy per connection.** Compromise of the passphrase and of both
  identity private keys after a connection has closed does not reveal that
  connection's traffic keys.
- **Authenticated handshake transcript.** Stream ID, transport settings and
  every extension record in both CONCLUSION messages are covered by both
  signatures. The later AGREEMENT message is covered by the caller signature.
- **Authenticated DATA.** REQUIRED needs AES-GCM, so identity is not followed
  by a payload that can be altered in transit.
- **Unchanged default.** With the option off, or in a build without the build
  option, handshake, key material and DATA bytes are identical to the current
  release.

Non-goals and limits that remain:

- Runtime controls (ACK, NAK, DROPREQ, KEEPALIVE, SHUTDOWN and others) stay
  unauthenticated. Only key-material controls and FEC parity are authenticated,
  as in session authentication version 2. A forged SHUTDOWN still closes the
  connection and a forged DROPREQ still discards data.
- AES-CTR is not available with peer authentication. AES-GCM keeps its
  documented limits: the retransmission bit is outside its authenticated header
  ([AES-GCM contract](aes-gcm-contract.md)).
- INDUCTION messages and the 16-byte SRT packet header of every handshake
  datagram are outside the transcript. IP addresses and ports are not bound.
- Identity privacy. Key identifiers travel in clear text.
- No certificates, expiry, names or revocation lists. Trust is the pinned set.
  Removing a key affects new connections only.
- No protection once an endpoint is compromised during a connection, and no
  recovery of secrecy after such a compromise.
- No post-quantum key agreement. Denial of service by an on-path attacker
  remains possible.
- Authorization stays an application decision. A valid identity is not
  permission to use a stream.

## Build option

Peer authentication is compiled only with
`ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_PEERAUTH=ON` (provisional name, default
`OFF`), the arrangement ACP1 uses while it is unreviewed.

- In a build without it, every socket option in this document returns
  `SRT_EINVOP`, as the ACP1 options do without their gate. The `0x7f16` record
  is skipped like any unknown record, and no new provider operation or
  dependency requirement exists. Ordinary builds keep today's Mbed TLS module
  list and today's wire bytes.
- It requires `ENABLE_AEAD_API_PREVIEW=ON`, because version 1 requires AES-GCM.
  Configuration fails otherwise.
- The elliptic-curve requirements listed under [Why P-256](#why-p-256) are
  checked at configure time only when the option is on.
- It is propagated through the installed CMake target and pkg-config `Cflags`,
  as the ACP1 and AES-GCM options are. Both peers must be built with it.

## Compatibility boundary

The option takes `ROBOTWEAX_SRT_EXTENSION_OFF` or `_REQUIRED`, the policy
values introduced for [path identifiers](group-path-identifiers.md).
`ROBOTWEAX_SRT_EXTENSION_IF_OFFERED` is rejected with `SRT_EINVPARAM` in
version 1: a listener that silently serves unauthenticated callers on the same
policy is the fallback this feature must not have.

| Caller | Listener child | Result |
| --- | --- | --- |
| OFF or stock | OFF or stock | Unchanged handshake, key material and DATA bytes |
| OFF or stock | REQUIRED | Listener rejects setup |
| Session authentication v2 only | REQUIRED | Listener rejects setup; a `0x7f10` offer never substitutes |
| REQUIRED | REQUIRED | Peer authentication active in both directions |
| REQUIRED | OFF | Setup fails; the caller receives no response record |
| REQUIRED | Session authentication v2 only | Setup fails; the caller sends no `0x7f10` offer |
| REQUIRED | Stock | Setup fails; interoperability is not claimed |

There is no fallback, no Prefer mode and no version negotiation. A REQUIRED
endpoint that does not complete the full exchange with a pinned peer fails
setup and exposes no application I/O. A stock listener that receives an offer
may ignore it, reject it or time out; the caller fails in every case.

A listener serves mixed populations the way it does for session authentication:
the listen callback selects REQUIRED or OFF on each provisional child, for
example by Stream ID, and the caller's offer must match that final value in
both directions. The callback's choice is explicit application policy, not
negotiation.

Prerequisites are `SRTO_ROBOTWEAX_SESSIONAUTH=true`, a passphrase, enforced
encryption and an explicit `SRTO_CRYPTOMODE=2` (AES-GCM) on both peers,
following the ACP1 precedent that a stronger profile does not enable its
prerequisites implicitly. Missing prerequisites fail connect or listen with
`SRT_ESECFAIL`; AUTO and AES-CTR are not accepted.

AES-GCM is required because proving who the peer is has little value if the
payload can then be modified in transit. It has been a released, default-off
extension since 0.2.0 ([AES-GCM contract](aes-gcm-contract.md)), so this adds
no unreleased dependency. Peer authentication is consequently limited to the
transport bundles that contract allows; for example File/Stream with FEC stays
invalid.

## A new record, not a new `0x7f10` version

Peer authentication uses a new CONFIG record, `0x7f16`. When it is active the
`0x7f10` record is not emitted. The reasons:

- Every `0x7f10` version (2 to 6) has the same fixed 100-byte content, and the
  parser rejects other sizes. This exchange needs three different layouts of
  136, 232 and 104 bytes.
- Versions 3 to 6 already encode ACP1 crossed with Caller/Listener,
  Rendezvous and groups. Identity is an independent axis; a shared number space
  would double that matrix.
- The proofs differ in kind. `0x7f10` carries one HMAC under a
  passphrase-derived root. This record carries key shares, a signature and a
  key-confirmation tag under an ephemeral secret.
- Both choices fail closed against older peers. A separate record keeps the
  version-2 parser and its known-answer vectors untouched.

The runtime half of session authentication version 2 is kept: authenticated
KMREQ/KMRSP subtypes `0x7f03`/`0x7f04` and authenticated FEC parity retain
their layouts, counters and replay windows, keyed as described under
[Key schedule](#key-schedule). The cost is one more private identifier.

## Cryptographic building blocks

No new primitive is defined. The exchange is the three-message sign-and-MAC
pattern analysed as
[SIGMA](https://doi.org/10.1007/978-3-540-45146-4_24) and used by
[IKEv2](https://www.rfc-editor.org/rfc/rfc7296) and
[TLS 1.3](https://www.rfc-editor.org/rfc/rfc8446), with raw public keys pinned
out of band as in [RFC 7250](https://www.rfc-editor.org/rfc/rfc7250). The
[key schedule](#key-schedule) is built from HKDF only, but the order in which
it mixes the passphrase departs from TLS 1.3; that departure is stated there
and is a named review item.

| Purpose | Suite 1 |
| --- | --- |
| Key agreement | ECDH on NIST P-256 with fresh ephemeral keys, [SP 800-56A Rev. 3](https://csrc.nist.gov/pubs/sp/800/56/a/r3/final) section 5.7.1.2; shared secret `Z` is the 32-byte big-endian x-coordinate |
| Signature | ECDSA on P-256 with SHA-256, [FIPS 186-5](https://csrc.nist.gov/pubs/fips/186-5/final); nonce generation as recommended under [Signatures and confirmation](#signatures-and-confirmation) |
| Key derivation | HKDF-SHA256, [RFC 5869](https://www.rfc-editor.org/rfc/rfc5869) |
| Tags | HMAC-SHA256, [RFC 2104](https://www.rfc-editor.org/rfc/rfc2104) |
| Transcript hash | SHA-256, [FIPS 180-4](https://csrc.nist.gov/pubs/fips/180-4/upd1/final) |
| Passphrase stretching | PBKDF2-HMAC-SHA1, 2048 iterations, [RFC 8018](https://www.rfc-editor.org/rfc/rfc8018), the existing provider primitive |
| Key wrap | AES Key Wrap, [RFC 3394](https://www.rfc-editor.org/rfc/rfc3394), the existing provider primitive |

Public keys are 64 bytes: the affine coordinates `X || Y`, each 32 bytes
big-endian, without the SEC 1 `04` prefix. Signatures are 64 bytes: `r || s`,
each 32 bytes big-endian. A *key identifier* is SHA-256 over the 64-byte
identity public key. Every received public key is validated as in SP 800-56A
section 5.6.2.3.3 before use: coordinates in range, on the curve, not the
point at infinity.

### Why P-256

What the repository establishes:

- Behind the peer-authentication gate, `CryptoProvider` exposes SHA-256,
  private-key generation/import, public-key validation and signature
  verification. `P256Key` owns a private key and exposes only its public key,
  ECDH and signing. The OpenSSL implementation is in
  `src/crypto_openssl_p256.hpp`; Mbed TLS and BCrypt still lack P-256 operations.
  HKDF is built from the existing HMAC-SHA256 primitive.
- OpenSSL 3.0 or later is required by `CMakeLists.txt`.
- The Mbed TLS backend accepts 3.6.7 and later 3.6.x, links only `mbedcrypto`,
  and rejects `MBEDTLS_USE_PSA_CRYPTO`
  ([Cryptographic backends](crypto-backends.md)). Its required-module list has
  no elliptic-curve module.
- The [BCrypt backend](windows-bcrypt.md) links the system `bcrypt` library.

P-256 is now exercised with OpenSSL, but **cross-backend portability is not
verified**. The remaining backend assumptions must be confirmed by implementation
and known-answer tests:

- OpenSSL uses `EVP_PKEY` for ECDH and ECDSA. Scalar import validates the range
  and uses OpenSSL's constant-time point multiplication to derive the public
  point. Imported public keys undergo a full provider public-key check.
- Mbed TLS 3.6 provides both through its legacy `mbedtls_ecdh_*` and
  `mbedtls_ecdsa_*` interfaces when `MBEDTLS_ECP_C`, `MBEDTLS_ECDH_C`,
  `MBEDTLS_ECDSA_C` and `MBEDTLS_ECP_DP_SECP256R1_ENABLED` are set. Those
  will become required modules when that backend is implemented and the build
  option is on, checked at configure time like the others. Builds without the
  option keep the current list.
- OpenSSL needs no newer version than the 3.0 already required, but it can be
  built without elliptic curves, so the build option adds a configure-time
  probe. BCrypt needs no additional library.
- Windows CNG provides `BCRYPT_ECDH_P256_ALGORITHM` and
  `BCRYPT_ECDSA_P256_ALGORITHM`, and its native signature format is the
  fixed-width `r || s` used here.
- Ed25519 signatures are absent from the Mbed TLS 3.6 legacy interface and
  from CNG. That is why X25519 with Ed25519 was not chosen: only P-256 offers
  both key agreement and signatures on all three backends with one curve.
- CNG can produce `HMAC(salt, Z)` from a secret-agreement handle, or export `Z`
  itself on current Windows versions. The byte order of an exported secret
  needs a test vector before the backend is trusted.

The new provider operations are optional and default to `Error::unsupported`,
as `hmac_sha256` does, so existing custom providers fail closed. A provider
that supplies a private-key handle must implement its public-key export, ECDH
and signing operations. Failed output-producing operations erase their output.
HKDF needs no provider change; it is built from `hmac_sha256`.

## Handshake flow

INDUCTION is unchanged. The exchange maps onto the three messages that session
authentication already uses, with no added round trip:

| Message | Direction | Carries |
| --- | --- | --- |
| M1, CONCLUSION offer | Caller to listener | Offer record; no key-material record |
| M2, CONCLUSION response | Listener to caller | Response record |
| M3, AGREEMENT | Caller to listener | Confirmation record |

All integers are network byte order. The record requires the CONFIG bit, is
emitted after the existing private records in canonical order, and is rejected
if duplicated. Every layout starts with the same 8-byte prefix:

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 1 | Format version, 1 |
| 1 | 1 | Message: 1 offer, 2 response, 3 confirmation |
| 2 | 1 | Suite, 1 |
| 3 | 1 | Flags, zero |
| 4 | 1 | Cipher mode: 2, AES-GCM; every other value fails decoding in version 1 |
| 5 | 1 | Traffic key length in bytes: 16, 24 or 32; an offer may carry 0 for no preference |
| 6 | 2 | Reserved, zero |

**Offer**, 136 bytes (34 words):

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | Prefix |
| 8 | 32 | Caller nonce |
| 40 | 64 | Caller ephemeral public key |
| 104 | 32 | Caller key identifier |

**Response**, 232 bytes (58 words):

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | Prefix, carrying the selected cipher mode and key length |
| 8 | 32 | Listener nonce |
| 40 | 64 | Listener ephemeral public key |
| 104 | 32 | Listener key identifier |
| 136 | 64 | Listener signature |
| 200 | 32 | Listener confirmation tag |

**Confirmation**, 104 bytes (26 words):

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | Prefix, repeating the selected cipher mode and key length |
| 8 | 64 | Caller signature |
| 72 | 32 | Caller confirmation tag |

Nonces and ephemeral keys are fresh for every connection attempt and come from
the provider's random generator. Nonces are carried in addition to the key
shares so that a later version could reuse a listener key share without losing
freshness; version 1 never reuses one.

Cipher and key-length negotiation moves into the prefix, because M1 carries no
key material. Both peers have set AES-GCM explicitly, as Rendezvous GCM already
requires, so the cipher mode is 2 in every message. The response key length is
the caller's nonzero value, otherwise the listener's configured
`SRTO_PBKEYLEN`, otherwise 16. The key length advertised in INDUCTION is not used, so an
attacker who rewrites INDUCTION cannot steer it. The handshake encryption field
keeps its meaning and is covered by the transcript; a value that contradicts
the prefix fails setup.

Under REQUIRED, a wrong size for the message type, an unknown version, suite or
message value, nonzero flags or reserved bytes, an all-zero nonce, an invalid
public key, a duplicate record or a missing CONFIG bit fail decoding. With
policy OFF the record is not parsed and is skipped like any unknown record.

### Transcript

For a handshake message `M`, `T(M)` is its 48-byte handshake control
information field followed by every extension byte to the end of the datagram,
exactly as carried, with the signature and confirmation-tag fields of the
`0x7f16` record replaced by zeros. `M2*` is the same byte range of M2 without
that replacement. The 16-byte SRT packet header is excluded because its
timestamp changes on a retry.

```text
label = ASCII "RWSRT-PEERAUTH-1"                       (16 bytes)
th_L  = SHA-256(label || len(T(M1)) || T(M1) || len(T(M2)) || T(M2))
th_C  = SHA-256(label || len(T(M1)) || T(M1) || len(M2*)   || M2*
                      || len(T(M3)) || T(M3))
```

Each `len` is a uint32 byte count. The hash therefore covers, for M1 to M3:
handshake version, encryption field, extension flags, initial sequence number,
MSS, flow window, handshake type, socket IDs, cookie and address field; HSREQ
and HSRSP; Stream ID; congestion and packet-filter configuration; group
membership; and every private record, including records the receiver does not
understand. It also covers both nonces, both key shares, both key identifiers
and the negotiated cipher.

This closes the unauthenticated-transcript gap stated in the session
authentication and ACP1 documents for the CONCLUSION and AGREEMENT messages.
It does not cover INDUCTION or packet headers, as listed under non-goals. Any
value that INDUCTION influences must reappear in M1 or M2 to be authenticated.

A message that is retransmitted must repeat the same `T(M)` bytes. In
`src/handshake.cpp` a retry is not a replay of stored bytes: the caller
rebuilds its CONCLUSION from its fixed configuration, and the listener response
and the caller AGREEMENT are re-sent from a cached action, each encoded again.
Byte identity therefore depends on the encoder being deterministic and on the
compatibility layer adding the same fields each time, which remains an
*assumption*. An implementation should keep the encoded bytes of each message
for the transcript and for retries.

### Key schedule

The schedule has two stages. The first depends only on the ephemeral secret.
The second adds the passphrase, and neither side computes it until the other
has proven a pinned identity.

```text
hs      = HKDF-Extract(salt = 32 zero bytes, IKM = Z)
hkey(n) = HKDF-Expand(hs, info = label || uint8(n) || th_L, 32 bytes)

psk     = PBKDF2-HMAC-SHA1(passphrase,
              salt = label || caller nonce || listener nonce, 2048, 32 bytes)
master  = HKDF-Extract(salt = hkey(2), IKM = psk)
key(n)  = HKDF-Expand(master, info = label || uint8(n) || th_C, length)
```

| Key | Depends on | Length | Use |
| --- | --- | --- | --- |
| `hkey(1)` | Ephemeral secret | 32 | Listener confirmation key |
| `hkey(2)` | Ephemeral secret | 32 | Salt that chains the first stage into the second |
| `key(3)` | Ephemeral secret and passphrase | 32 | Caller confirmation key |
| `key(4)` | Ephemeral secret and passphrase | 32 | Session-authentication key for KMREQ/KMRSP and FEC parity tags |
| `key(5)` | Ephemeral secret and passphrase | Traffic key length | Key-encrypting key for key material sent by the caller |
| `key(6)` | Ephemeral secret and passphrase | Traffic key length | Key-encrypting key for key material sent by the listener |

Every expansion is at most 32 bytes, a single HMAC block. Chaining two
extractions through a derived salt is the pattern of the TLS 1.3 key schedule
(RFC 8446 section 7.1).

**Departure from the cited schedule.** TLS 1.3 extracts the pre-shared key
first and the ECDHE secret second, so its first authenticated flight already
depends on the pre-shared key. This draft reverses the order: the ephemeral
secret first, the passphrase second. The purpose is that M2, which a listener
sends to a caller it has not yet authenticated, contains nothing derived from
the passphrase. Mixing a pre-shared key late in an exchange has precedent in
the `psk` modifiers of the
[Noise framework](https://noiseprotocol.org/noise.html), but this schedule is
not a Noise pattern and has no published analysis. It must be reviewed as a
construction in its own right, including the use of a PBKDF2 output as the
input keying material of the second extraction.

The PBKDF2 salt includes both nonces, as the salt of session authentication
version 2 does. Each side performs one derivation per handshake, and only
after the other side has authenticated, so an unauthenticated flood cannot
reach it.

### Signatures and confirmation

The signed message follows the framing of RFC 8446 section 4.4.3, which exists
to stop a signature from being reused in another protocol or role:

```text
signed_L  = 64 bytes of 0x20 || "Robotweax SRT peer authentication 1, listener"
            || 0x00 || th_L
signed_C  = 64 bytes of 0x20 || "Robotweax SRT peer authentication 1, caller"
            || 0x00 || th_C
sig_L     = ECDSA-P256-SHA256(listener identity key, signed_L)
sig_C     = ECDSA-P256-SHA256(caller identity key, signed_C)
confirm_L = HMAC-SHA256(hkey(1), th_L || sig_L)
confirm_C = HMAC-SHA256(key(3),  th_C || sig_C)
```

Processing, with failure ending setup without fallback except for the bounded
invalid-M3-signature allowance described below:

1. **Listener, on M1.** After the cookie check, the listen callback and the
   admission charge described below: look up the caller key identifier in the
   child's pinned set, validate the caller key share, generate its own key
   share, compute `Z`, `hs`, `hkey(1)` and `hkey(2)`, sign, and send M2. It
   keeps `hkey(2)` and the transcript for the pending child and erases its
   ephemeral private key, `Z` and `hs`. It does not use the passphrase.
2. **Caller, on M2.** Look up the listener key identifier in its pinned set,
   validate the key share, compute `Z` and the first stage, check `confirm_L`,
   then verify `sig_L`. Only after both succeed does it compute `psk`, `master`
   and the second-stage keys, sign, and send M3. It erases its ephemeral
   private key, `Z` and `hs`.
3. **Listener, on M3.** Verify `sig_C` with the pinned key selected in step 1.
   Only if the signature is valid does it compute `psk` and `master` and check
   `confirm_C`. Only then is the connection published to `accept`.

On both peers, `hkey(2)` and `psk` must be erased immediately after deriving
`master`. The listener retains `hkey(2)` only while waiting for M3; retries
use cached handshake messages and do not require retaining it after derivation.
A retained `hkey(2)`, together with a later passphrase compromise and a recorded
transcript, would allow reconstruction of `master` and the traffic wrapping
keys.

The order in step 3 is deliberate. A sender without the pinned caller key
fails at the signature, so the passphrase-dependent comparison is never
evaluated for it and its outcome cannot leak through timing or through the
rejection reason. All failures in step 3 use one rejection reason.

The Stream ID a listen callback reads at step 1 is not yet authenticated. It
is authenticated, together with the rest of M1, when step 3 succeeds, which is
before the application can accept the connection.

The listener retries M2 until M3 arrives, and a connected caller resends its
cached M3 for a matching M2, as session authentication does today. A repeated
identical M1 gets the cached M2 without new public-key work. An M1 that changes
any byte of an exchange in progress is ignored.

**Signature nonces.** The listener signs on demand for callers it has not
authenticated, so a biased or repeated ECDSA nonce would expose its identity
key. Implementations should use deterministic nonces
([RFC 6979](https://www.rfc-editor.org/rfc/rfc6979)), or hedged nonces that
mix the private key and the message with fresh randomness, wherever the
backend offers them, and must never use a nonce source weaker than the
provider's random generator. Deterministic and randomized signatures verify
identically, so the choice is local to each endpoint. The OpenSSL implementation
requests RFC 6979 when its signature provider advertises the nonce-type
parameter; otherwise it uses the provider's default nonce generation. It does
not implement its own nonce generator. Remaining backend *assumptions* from
library documentation: Mbed TLS 3.6 offers RFC 6979 through
`MBEDTLS_ECDSA_DETERMINISTIC`; OpenSSL offers it as a signature parameter from
3.2, later than the 3.0 minimum, and otherwise mixes the private key and digest
into its random nonces; CNG exposes no nonce control and draws from the system
generator.

### What each proof establishes

| Proof | Establishes | Passphrase involved |
| --- | --- | --- |
| `sig_L` | The listener holds a pinned identity key and signed M1 and M2 | No |
| `confirm_L` | That signer derived `Z`, which binds its identity to this key agreement | No |
| `sig_C` | The caller holds a pinned identity key and signed M1, M2 as received and M3 | No |
| `confirm_C` | The caller derived `Z` and holds the passphrase | Yes |
| First authenticated KMREQ or KMRSP from the listener | The listener holds the passphrase | Yes |

Both key identifiers are inside the transcript, so a third pinned peer cannot
re-sign another peer's exchange as its own.

The handshake has three messages, so the listener cannot prove knowledge of
the passphrase inside it without reintroducing the exposure this schedule
removes. The caller obtains that proof from the first key-material control,
whose tag needs `key(4)`. No DATA is released and no traffic key is installed
before it. *Proposed:* a connection whose first authenticated key exchange has
not completed within the connect timeout is closed.

Residual passphrase-guessing exposure:

- **No identity key.** None. M1 and M2 are independent of the passphrase, a
  caller sends M3 only to a listener whose signature it verified, and a
  listener evaluates `confirm_C` only after a valid caller signature.
- **A stolen caller identity key that a listener pins.** Online guessing only:
  one guess per handshake, each a fresh exchange charged to the admission
  budget. The listener can observe it as valid signatures followed by failed
  confirmation.
- **A stolen listener identity key, used to impersonate that listener to a
  genuine caller.** Offline guessing. The impostor receives the caller's M3,
  knows `Z`, and can test guesses against `confirm_C`. A high-entropy
  passphrase is the only defence, as for any pre-shared key
  ([RFC 9257](https://www.rfc-editor.org/rfc/rfc9257)).
- **A pinned peer** holds the passphrase by configuration.

### Datagram budget

The SRT handshake is one datagram and is never fragmented or truncated; a
handshake that does not fit fails encoding. Sizes below include each record's
4-byte header. The budget is the MSS less 48 bytes, the IPv6 and UDP allowance
the other private records already reserve.

| Component | M1 | M2 | M3 |
| --- | --- | --- | --- |
| SRT header and handshake field | 64 | 64 | 64 |
| HSREQ or HSRSP | 16 | 16 | 0 |
| Peer-authentication record | 140 | 236 | 108 |
| **Minimum total** | **220** | **316** | **172** |
| Stream ID, at most | 516 | 0 | 0 |
| Congestion (`control-v1`), at most | 16 | 16 | 0 |
| Packet filter, at most | 516 | 516 | 0 |
| Null packet deletion, if that draft is adopted | 12 | 12 | 12 |
| **Maximum total, version 1 scope** | **1,280** | **860** | **184** |

At the default MSS of 1,500 the budget is 1,452 bytes, so the largest version 1
handshake fits with 172 bytes to spare. The implementation-identity record is
added only when it fits and is otherwise omitted, as today. For comparison,
session authentication version 2 spends up to 180 bytes in M1 on its record and
an AES-256 key-material record; this draft spends 140.

Two entries in the table were checked in `src/handshake.cpp`. Only caller
messages set a Stream ID, so the response carries none. AGREEMENT carries no
HSREQ: it is built with the CONFIG flag alone and attaches the
session-authentication record, plus the path-identifier and standby-RTT
records when those are required.

`SRTO_MSS` accepts values down to 76, which cannot hold even M1. *Proposed:* a
REQUIRED policy needs an MSS of at least 512, enforced in either setting order
as ACP1 enforces 256, and negotiation cannot lower a connection below it. At
512 the budget is 464 bytes, leaving 148 bytes for optional records in M2. At
the IPv6 minimum MTU of 1,280 the budget is 1,232 bytes, so a maximum-length
Stream ID and a maximum-length filter configuration cannot both be sent.

## Traffic keys, forward secrecy and rotation

The key-material message keeps its existing byte layout. What changes is the
key that wraps the traffic keys inside it.

- No key-material record is sent in CONCLUSION. The caller cannot wrap a key
  under the ephemeral secret before it has the listener's key share, and a key
  wrapped under the passphrase alone would defeat the purpose.
- After the handshake each endpoint generates its own random transmit key and
  salt and announces them with the runtime key exchange that Robotweax peers
  already run before sending DATA ([Encryption](encryption.md)).
  `SRTO_SNDKMSTATE` reports SECURING until the peer confirms. That this
  exchange can start without handshake key material is an *assumption* about
  the current implementation.
- The traffic keys in every KMREQ are wrapped with RFC 3394 under `key(5)` or
  `key(6)`, by sender role. The passphrase-derived key-encrypting key is never
  used. The salt field keeps its role in IV derivation only, so a receiver
  performs no PBKDF2 for a runtime KMREQ.
- KMREQ and KMRSP use subtypes `0x7f03` and `0x7f04` with the version-2 layout:
  uint64 counter, key-material body, 32-byte tag. The tag key is `key(4)` and
  the MAC label is the 16-byte `label` above; domain bytes, role byte, counter
  rules and retry rules are those of version 2. Bare KMREQ and KMRSP are
  ignored.
- Rotation follows `SRTO_KMREFRESHRATE` and `SRTO_KMPREANNOUNCE` unchanged.
  Each new traffic key is random and wrapped under the same directional
  key-encrypting key for the life of the connection.
- The first exchange in each direction also completes the passphrase proof
  described under [What each proof establishes](#what-each-proof-establishes).
- On setup failure, timeout, cancellation or close, both peers must erase all
  remaining per-connection secrets: the ephemeral private key, `Z`, `hs`,
  every `hkey(n)`, `psk`, `master`, every `key(n)` and all traffic keys,
  including temporary copies.

Forward secrecy is therefore per connection. A recorded connection stays
confidential if the passphrase and both identity keys leak after it closed and
the endpoints erased their state. An endpoint compromised while the connection
is open exposes that connection's key-encrypting keys and with them every
traffic key of that connection, past and future.

## Listener cost and denial of service

Public-key work is placed after every cheaper check:

1. INDUCTION stays stateless and does no peer-authentication work.
2. A CONCLUSION must carry a valid cookie, which the listener binds to the
   caller endpoint, socket ID, initial sequence number, MSS and flow window
   ([Protocol edge cases](protocol-edge-cases.md)). This is the proof of
   reachability.
3. The record is parsed and the caller key identifier is checked against the
   child's pinned set after the listen callback. An unknown identifier is
   rejected with no public-key operation. Identifiers are public, so this stops
   scanners, not a determined attacker.
4. The existing `ListenerAdmissionBudget` is charged before crypto setup, as it
   is today for every cookie-valid conclusion including repeats: 32 tokens per
   source address refilled at 16 per second, and 128 tokens overall refilled at
   64 per second. Accept capacity is reserved before setup.
5. Only then does the listener validate one public key, generate one key pair,
   run one ECDH and produce one signature.
6. M3 is matched to a pending child by endpoint, socket IDs and cookie. The
   signature is verified before the confirmation tag, for the reason given
   under [Signatures and confirmation](#signatures-and-confirmation), so each
   M3 that reaches a pending child costs one verification. The private exchange
   currently fails after four M3 messages with an invalid signature; this limit
   remains subject to review. A valid signature with an invalid confirmation
   ends the attempt immediately. Identical accepted M3 retries do no new crypto
   work.
7. PBKDF2 runs only after a valid caller signature, so no unauthenticated
   sender can cause one.

A caller performs its public-key work only for a response that matches its own
pending attempt. Invalid responses are dropped and the attempt ends at its
connect timeout.

The response record is 96 bytes larger than the offer record, and M2 is sent
only to an address that returned a cookie, so the exchange adds no useful
amplification.

*Open:* whether public-key admissions should draw from their own budget
instance or cost more than one token. At 64 admissions per second the work is
small with an optimized P-256 implementation, but a portable one may need
several milliseconds per operation. No measurement exists yet; this is an
*assumption* to replace with numbers on each backend.

## Interaction with other features

- **Session authentication version 2.** A prerequisite option, replaced on the
  wire. The `0x7f10` record and its PBKDF2 session root are not used. The
  version-2 KMREQ/KMRSP and FEC parity protections continue under `key(4)`.
- **ACP1.** Not part of version 1. REQUIRED together with
  `SRTO_ROBOTWEAX_ACP1_REQUIRED` fails in either setting order. The composition
  is the [first follow-up](#first-follow-up-authenticated-runtime-controls).
- **AES-GCM.** Required. The DATA format, IV derivation and tag are those of
  the [AES-GCM contract](aes-gcm-contract.md), unchanged. The key length is
  authenticated by the transcript.
- **AES-CTR.** Not available with REQUIRED. With policy OFF it is unchanged.
- **FEC parity authentication.** The version-2 envelope, counter and 64-packet
  replay window are kept, keyed by `key(4)` with the `label` above. The 40-byte
  envelope is reserved when a packet filter is enabled.
- **Rendezvous.** Out of scope for version 1. REQUIRED with `SRTO_RENDEZVOUS`
  fails in either order, as session authentication alone does.
- **Connection groups.** Out of scope for version 1. Group handles return
  `SRT_EINVOP`, and REQUIRED with `SRTO_GROUPCONNECT` fails. A later version
  must also require every member of one accepted group to authenticate as the
  same peer key.
- **Multicast dispatch groups.** Out of scope for version 1. One traffic key is
  shared by all receivers there, so neither sender identity among receivers nor
  secrecy across membership changes would follow from this exchange.
- **Other private records.** Path identifiers, standby RTT and implementation
  identity are group or diagnostic features. Where they appear they are covered
  by the transcript as bytes.
- **Null packet deletion.** That
  [draft](https://github.com/Robotweax/srt/pull/384) notes that a forged echo
  goes undetected. Under peer authentication its record is inside the
  transcript, so removal or forgery fails setup.

## Socket options and key handling

Proposed private options, all provisional. Each returns `SRT_EINVOP` in a build
without the [build option](#build-option):

| Option | Contract |
| --- | --- |
| `SRTO_ROBOTWEAX_PEERAUTH_POLICY` (`0x01000019`) | PRE `int32_t`, default OFF: OFF or REQUIRED; inherited by accepted sockets; a listen callback may override it on the provisional child |
| `SRTO_ROBOTWEAX_PEERAUTH_KEY` (`0x0100001a`) | PRE, write-only: the local identity private key, a 32-byte big-endian P-256 scalar; erased on close |
| `SRTO_ROBOTWEAX_PEERAUTH_TRUSTED` (`0x0100001b`) | PRE: one to sixteen pinned peer public keys, 64 bytes each, concatenated; setting replaces the set; a listen callback may replace it on the provisional child |
| `SRTO_ROBOTWEAX_PEERAUTH_OFFERED` (`0x0100001c`) | Read-only in a listen callback: the 32-byte key identifier the caller claims, not yet authenticated |
| `SRTO_ROBOTWEAX_PEERAUTH_PEER` (`0x0100001d`) | Read-only on a connected socket: the 64-byte public key the peer authenticated with |

REQUIRED without a private key, with an empty pinned set or without AES-GCM
fails connect or listen with `SRT_ESECFAIL`. A malformed key or a public key that is not on the
curve fails the setter with `SRT_EINVPARAM`. Connected sockets reject changes.

```c
int policy = ROBOTWEAX_SRT_EXTENSION_REQUIRED;
/* Check every return value. Session authentication, a passphrase, enforced
   encryption and SRTO_CRYPTOMODE=2 must be configured as well. */
srt_setsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_KEY, private_key, 32);
srt_setsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_TRUSTED, peer_public_key, 64);
srt_setsockflag(socket, SRTO_ROBOTWEAX_PEERAUTH_POLICY, &policy, sizeof(policy));
```

Pinning is the whole trust model. The application obtains each peer's public
key out of band and decides which keys may connect. A caller normally pins one
listener key, or two while the listener changes its key. A listener with a
large population reads `SRTO_ROBOTWEAX_PEERAUTH_OFFERED` in its listen
callback, looks the identifier up in its own store, and pins that single key on
the child; the exchange then proves or refutes the claim. After `accept`, the
application reads `SRTO_ROBOTWEAX_PEERAUTH_PEER` and applies its authorization
rules to the authenticated key and the authenticated Stream ID.

A key is rotated by pinning the old and new keys together for a transition
period. A key is revoked by removing it from the set, which does not close
existing connections. Private keys must come from a cryptographic random
generator and be stored like any other long-term secret.

## Alternatives considered

- **X.509 certificate chains.** Not chosen. The handshake is one datagram of at
  most 1,452 bytes shared with up to 1,032 bytes of Stream ID and filter
  configuration, and has no fragmentation. A leaf certificate with one
  intermediate does not fit reliably. The Mbed TLS backend links no X.509
  library, and chain validation would add trust stores, wall-clock time and
  revocation to a transport library.
- **SRP** ([RFC 2945](https://www.rfc-editor.org/rfc/rfc2945),
  [RFC 5054](https://www.rfc-editor.org/rfc/rfc5054)). Not chosen. Its message
  order needs a fourth handshake message for mutual confirmation, which the
  CONCLUSION, CONCLUSION, AGREEMENT exchange does not have. It also proves
  knowledge of a password verifier, not possession of a per-peer key, and needs
  big-number arithmetic that the provider interface does not expose.
- **A static Diffie-Hellman pattern without signatures**, as in the Noise
  framework. Not chosen. It would need only ECDH, but Noise defines its
  patterns for Curve25519 and Curve448; using them with P-256 would be a
  nonstandard combination.
- **Running DTLS around SRT.** Not a handshake extension. An application can
  already place SRT inside another secured tunnel; this draft is for
  deployments that cannot.

## First follow-up: authenticated runtime controls

Version 1 leaves a connection between two authenticated peers open to forged
runtime controls. The first follow-up is therefore the composition with
[ACP1](experimental-acp1.md), ahead of Rendezvous and groups:

- ACP1's two direction keys would be expanded from `master` with further key
  numbers, in place of its expansion from the PBKDF2 session root. Its
  envelope, counters and replay window would be unchanged.
- ACP1 is selected today by the version number in the `0x7f10` record, which
  peer authentication does not emit. The composition needs its own selector,
  for example a flag in the `0x7f16` prefix, covered by the transcript.
- ACP1 is itself experimental and unreviewed
  ([issue #190](https://github.com/Robotweax/srt/issues/190)). The composition
  needs its own review and cannot be claimed from the two parts.

Until it exists, applications that need protection against forged SHUTDOWN,
ACK, NAK or DROPREQ must not rely on peer authentication for it.

## Open decisions

1. **A mode without a passphrase.** Version 1 requires one as a second factor.
   Identity-only operation would need the encryption options to work without
   a passphrase.
2. **`IF_OFFERED` on listeners.** Excluded here. Allowing it would need a rule
   that applications must read the per-connection result.
3. **Caller key identifier in M1.** It enables the callback lookup and the
   cheap rejection, and discloses which key is connecting. The alternative
   sends it only in M3.
4. **Admission cost** of public-key work, and whether a listener may prepare
   key shares ahead of demand.
5. **Limit of four invalid M3 signatures** per pending child, and a counter
   that lets an application see repeated confirmation failures after valid
   signatures.
6. **Closing a connection** whose first authenticated key exchange does not
   complete within the connect timeout.
7. **Minimum MSS** of 512.
8. **Marking key material.** Whether a nonzero KEKI should flag key material
   wrapped under an ephemeral key-encrypting key. Key material uses KEKI zero
   today.
9. **Rekeying the key-encrypting key** during long connections, to limit what
   a compromise during a connection exposes.
10. **Binding INDUCTION.** Whether any INDUCTION field beyond the key length
    needs to be restated in M1 or M2.
11. **Private keys that cannot be exported.** A signing callback for hardware
    or operating-system key stores, and a helper to generate and convert keys.
12. **Pinned-set limit** of sixteen keys.
13. **Rejection reasons.** Session authentication uses `SRT_REJ_UNSECURE` and
    `SRT_REJ_BADSECRET`. This draft uses one reason for every M3 failure;
    whether M1 and M2 failures should share it, so that a listener does not
    reveal which identifiers it pins.
14. **Build option name** and whether it should also gate the provider
    interface additions.
15. **Later scope,** after the first follow-up: Rendezvous, connection groups
    and multicast dispatch groups.

## Review and qualification required before any support claim

Independent cryptographic review comes first and covers at least:

- the two-stage key schedule, its reversal of the TLS 1.3 extraction order,
  and the use of a PBKDF2 output as input keying material;
- the claim that nothing sent before the peer has authenticated depends on
  the passphrase, including the check order and single rejection reason in
  step 3;
- the listener's passphrase proof being deferred to the first key exchange;
- the transcript definition and the zeroed-field rule;
- identity binding and role separation;
- downgrade paths through INDUCTION and through listener policy;
- nonce and key-share freshness, signature nonce generation and key erasure;
- the listener cost model; and
- whether requiring AES-GCM while leaving runtime controls unauthenticated is
  an acceptable version 1 boundary.

The design should also be checked against the open questions recorded for ACP1
in [issue #190](https://github.com/Robotweax/srt/issues/190). This draft does not
close that issue or [issue #112](https://github.com/Robotweax/srt/issues/112),
and it was not part of the
[2026-11-04 source review](security-review-2026-11-04.md).

Evidence must then be reproducible and black-box, as required by
[Contributing](../CONTRIBUTING.md):

- Byte comparison showing that OFF/OFF connections, with and without session
  authentication version 2, are unchanged from the current release.
- Explicit failure of REQUIRED against each pinned reference build in both
  roles, recording whether the reference ignores, rejects or times out.
- Explicit failure for every mismatched row of the compatibility table, and
  for a missing key, an unpinned key, a wrong passphrase, a wrong suite and a
  cipher mode other than AES-GCM.
- A build without the build option: every option returns `SRT_EINVOP`, the
  dependency requirements are unchanged, and the wire bytes match the current
  release.
- With fixed randomness, M2 is byte-identical for two listeners that differ
  only in passphrase.
- A pinned caller with a wrong passphrase and an unpinned caller are both
  rejected, with the same reason, and the unpinned caller's rejection time
  does not depend on its confirmation tag.
- A listener without the passphrase cannot complete the first key exchange,
  and the caller releases no DATA to it.
- Known-answer tests on all three backends: ECDH and ECDSA vectors from the
  NIST validation program and RFC 6979, HKDF vectors from RFC 5869, and a
  complete handshake vector computed by an independent implementation.
- Cross-backend peers in every pairing of OpenSSL, Mbed TLS and BCrypt, both
  roles, AES-CTR and AES-GCM, all key lengths, with FEC and repeated rotation.
- Tampering with every byte of M1, M2 and M3 on the wire, including Stream ID,
  filter configuration, MSS and unknown records; each must fail setup.
- Reflection, replay of recorded handshakes, role swap, substitution of another
  pinned peer's signature, and invalid curve points.
- Loss and retry of each message, including a lost AGREEMENT, without new
  public-key work for identical retries.
- A recorded connection that cannot be decrypted with the passphrase and both
  identity private keys.
- Listener load tests under cookie-valid floods on each backend, showing that
  the admission budget bounds CPU and that established connections continue.
- Fuzzing of the record parser and of the three-message state machine.
- Datagram-size tests at the minimum MSS and at the IPv6 minimum MTU.
