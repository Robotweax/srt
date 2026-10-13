# Experimental ACP1 runtime-control authentication

ACP1 is an **unreviewed, unstable prototype**, behind
`ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_ACP1=ON` (default `OFF`). It is not a supported
security profile or a release compatibility promise. The wire values below
are provisional. Independent cryptographic review and a separate go/no-go
remain outstanding under [issue #190](https://github.com/Robotweax/srt/issues/190).
This prototype does not close that issue or the legacy KM replay issue
[#112](https://github.com/Robotweax/srt/issues/112).

## Opt-in and compatibility

Build both peers explicitly:

```sh
cmake -S . -B build-acp1 -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_ACP1=ON
cmake --build build-acp1 --parallel
ctest --test-dir build-acp1 -R acp1 --output-on-failure
```

Enable both boolean options before connection setup, configure a high-entropy
passphrase, and keep enforced encryption enabled:

```c
bool enabled = true;
/* Check both return values; configure SRTO_PASSPHRASE securely as well. */
srt_setsockflag(socket, SRTO_ROBOTWEAX_SESSIONAUTH, &enabled, sizeof(enabled));
srt_setsockflag(socket, SRTO_ROBOTWEAX_ACP1_REQUIRED, &enabled, sizeof(enabled));
```

| Private option | Contract |
| --- | --- |
| `SRTO_ROBOTWEAX_ACP1_REQUIRED` (`0x0100000c`) | PRE boolean, default false; inherited by accepted sockets and group members |
| `SRTO_ROBOTWEAX_ACP1_ACTIVE` (`0x0100000d`) | Read-only boolean; established ACP1 connection, or required group with at least one connected member and every connected member ACTIVE |

Both options return `SRT_EINVOP` in builds without the experimental gate.
Enabling REQUIRED does not implicitly enable SESSIONAUTH, encryption or GCM;
missing prerequisites fail connect/listen with `SRT_ESECFAIL`. Listener
admission revalidates final callback settings. Required policy uses strict
negotiation with no Prefer mode or fallback. ACTIVE remains false during setup,
in a listener callback, on a listening socket and after failed/terminal setup.

The prototype supports ordinary Caller/Listener and Rendezvous sockets,
including synchronous and asynchronous setup, plus Broadcast and Backup
Caller/Listener groups. Set REQUIRED and SESSIONAUTH on the caller group and
GROUPCONNECT listener before opening them. Each join or replacement inherits
that group policy; endpoint ACP1 overrides must match it. A registered member
cannot change REQUIRED individually, and a listener callback cannot disable
its listener's required group policy. Mirrored groups reject joins across
ACP1 policies, including across bonded listeners. Group Rendezvous remains
outside the existing group transport's supported modes.

Group ACTIVE is false for an empty group or a group without REQUIRED. It is
true when at least one member is connected and every connected member reports
ACTIVE. Pending, broken and closed members do not count; surviving protected
members can keep the group ACTIVE during join, replacement or failover.
The query observes members individually and may conservatively return false
during concurrent closure. It is an observation, not an authorization token.
For Rendezvous, enable ACP1 before enabling SESSIONAUTH on an already configured
Rendezvous socket, or before enabling Rendezvous on a SESSIONAUTH socket.
SESSIONAUTH alone remains incompatible with Rendezvous. Disabling ACP1 while
both Rendezvous and SESSIONAUTH are enabled is rejected.
ACP1 requires MSS at least 256;
setting either MSS or ACP1 rejects an incompatible existing value. Negotiation
cannot lower an ACP1 connection below that limit.

With REQUIRED false, standard connections keep their existing API and wire
behavior, including Haivision 1.5.7 interoperability. SESSIONAUTH alone still
uses version 2 byte for byte. ACP1 requires version 3 for ordinary Caller/Listener,
version 4 for Rendezvous, or version 5 for group members on both peers: version-2
SESSIONAUTH peers and stock Haivision peers cannot establish protected
connections. These strict failures are part of the protection contract.
Existing exported C functions and `SRTO_E_SIZE` are unchanged. The build gate
is propagated through the installed CMake target and pkg-config Cflags.

## Threat model and coverage

The intended property is connection-bound integrity, sender-direction binding
and bounded replay rejection for runtime controls against a network attacker
without the passphrase. Authentication precedes reliability, key installation,
statistics and liveness mutations. Tags do not encrypt control content.

| Traffic | Experimental protection |
| --- | --- |
| ACK, ACKACK, NAK, DROPREQ | Complete original header and payload inside ACP1 envelope |
| KEEPALIVE, SHUTDOWN, PEERERROR, congestion warning | Same envelope, including normally unused header fields/padding |
| Runtime KMREQ/KMRSP | Existing session-bound KM counters/tag, additionally inside ACP1 |
| FEC parity | Existing separate session-bound FEC envelope, with the negotiated version-3/4/5 root/domain |
| Backup standby RTT request/echo (`0x7f12`) | Version-5 ACP1 envelope before monitoring state; no reliability/liveness mutation |
| Source DATA | Existing chosen cipher; ACP1 does not change the DATA wire format |
| Handshake | Existing nonce/socket-ID/role/initial-KM proofs, with the negotiated version-3/4/5 root/domain |

Nested envelopes, unknown envelope versions and other user-defined runtime
controls are rejected. New runtime subtypes must receive an explicit coverage
decision before they are enabled. Existing semantic/window validation still
applies after successful authentication; a valid MAC is not permission to
ignore packet bounds or sequence rules.

AES-CTR source DATA remains unauthenticated and malleable. An attacker can still
forge DATA or exploit its consequences, including eliciting authentic controls
from a receiver. Select AES-GCM separately (`ENABLE_AEAD_API_PREVIEW=ON`,
`SRTO_CRYPTOMODE=2`) for source-DATA integrity. ACP1 does not authenticate a
complete handshake transcript: Stream ID, implementation identity, transport
settings and other extension fields remain outside its proof. Neither CTR nor
ACP1 should be described as protecting the entire transport.

Other limits include denial of service, traffic analysis, packets withheld or
reordered beyond the replay window, malicious peers/passphrase holders, weak
passwords and compromised endpoints. Shared-secret holders can impersonate
one another; there is no forward secrecy. The existing 2048-iteration PBKDF is
not a password-storage recommendation. Do not use this experimental design as
a replacement for a reviewed network security layer.

## Session negotiation and key separation

All integers are network order. The existing `0x7f10` CONFIG extension remains
100 bytes: uint32 version, caller nonce[32], listener nonce[32], proof[32].
ACP1 selects **version 3**, with the same offer/challenge/AGREEMENT ordering as
[SESSIONAUTH version 2](session-authentication.md). Both nonces must be nonzero.
The caller verifies the listener proof; the listener verifies the final caller
proof before publication. Each endpoint requires its configured version.

Version 3 changes the root PBKDF salt and handshake/KM/FEC MAC label to the
16 bytes `RWSRT-SESSION-3` followed by NUL. The remaining salt inputs and
proof fields are unchanged: both 32-byte nonces, caller/listener socket IDs
encoded as uint64, sender role, proof domain and initial KM body. An attacker
cannot change the version to 2 while retaining a valid version-3 proof.

Rendezvous selects **version 4** in the same 100-byte CONFIG record. The
Initiator occupies the caller nonce/role position, and the Responder occupies
the listener position; these roles do not imply application DATA direction.
Version 4 uses `RWSRT-SESSION-4` followed by NUL in the root salt and all
handshake/KM/FEC MAC domains. Its salt is 112 bytes: the label, the two nonces,
Initiator and Responder socket IDs as uint64, then Initiator and Responder
cookies as uint64. Cookies must be nonzero and different. Version 2/3 roots do
not accept cookie arguments. Version 3 Caller/Listener wire bytes are unchanged.

WAVEAHAND and empty CONCLUSION retain their normal layouts. Once roles resolve,
the Initiator sends a version-4 nonce offer with HSREQ/KMREQ. The Responder
returns its nonce and proof with HSRSP/KMRSP. The Initiator verifies that proof
and sends its own proof in AGREEMENT. The Responder verifies it before becoming
connected. Retries keep the same nonces and initial KM; modified offers, proof
reflection, foreign-session proofs and version changes cannot complete setup.
No DATA or runtime control, even with a valid ACP1 MAC, substitutes for the
final handshake proof. An asynchronous ACP1 Rendezvous socket publishes
CONNECTED only after its protected runtime is installed; ACTIVE is then true.
Lost AGREEMENT is recovered by the Responder's authenticated HSRSP retry and
the connected Initiator's cached authenticated AGREEMENT.

The 32-byte PBKDF result is used as the pseudorandom key for two single-block
[HKDF-Expand](https://www.rfc-editor.org/rfc/rfc5869) operations:

```text
K_caller   = HMAC-SHA256(root, info16 || uint8(1) || uint8(1))
K_listener = HMAC-SHA256(root, info16 || uint8(2) || uint8(1))
info16     = ASCII("RWSRT-ACP1-1") || four zero bytes
```

For version 4, `K_caller` means Initiator and `K_listener` means Responder.
The last byte is the Expand block index. No additional Extract is performed;
review of using the PBKDF output directly as PRK is explicitly outstanding.
The direction keys are separate from DATA keys and from the root used by
handshake/KM/FEC domains. They remain fixed for the connection lifetime and are
securely erased with the session. Each new connection derives new keys from
fresh nonces and the socket identities.

## Group-member session binding

Broadcast and Backup members select **version 5** in the same CONFIG record.
Its root salt starts with `RWSRT-SESSION-5` and NUL, both 32-byte nonces and
caller/listener socket IDs encoded as uint64, followed by this canonical
88-byte group context. All integers use network order; absent fields are zero.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 8 | Caller membership: uint32 group ID, uint8 type, uint8 flags, uint16 weight |
| 8 | 8 | Listener membership with the same layout |
| 16 | 4 | Path-label byte length, zero if absent |
| 20 | 32 | Path-label bytes, padded with zeros |
| 52 | 4 | Standby identity present: uint32 0 or 1 |
| 56 | 16 | Caller standby nonce, zero if absent |
| 72 | 16 | Listener standby nonce, zero if absent |

The salt is 184 bytes. Handshake, KM and FEC MAC labels use version 5; directional
control-key expansion is unchanged. Both proofs therefore bind both memberships,
the path label and both standby identities through their member root. The
listener reserves its mirror membership and standby nonce before computing the
proof; rejection rolls the reservation back. The callback-only local GROUPDOMAIN
is never serialized. Stream ID and the rest of the handshake retain the limits
listed above.

Every member owns a fresh nonce exchange, root, two control keys, KM counters,
FEC counters and control replay window. No authentication state is shared with
the group's DATA sequence cursor. Joining or replacing a member creates fresh
state even when its path label and group ID are reused. DATA key rotation does
not reset that state. Valid controls from another member fail authentication.

Standby requests and echoes use the same per-member control counter/window as
other runtime controls. The monitoring send path seals before its direct UDP
send, preserves its skip-on-backpressure policy, and never queues probe retries.
Skipped sends consume counters; later controls are still accepted within the
normal bounded replay window. Versions 3 and 4 reject this group-only subtype.

## Runtime envelope

The envelope uses SRT user-defined control type `0x7fff`, provisional subtype
`0x7f05`. Its type-specific word is zero. Timestamp and destination socket ID
must equal those of the inner control.

```text
outer SRT control header[16]
uint32 envelope_version = 1
uint64 counter
original complete SRT control packet[16 + body_length]
HMAC-SHA256 tag[32]
```

This adds 60 bytes to the original datagram. The tag covers:

```text
ASCII("RWSRT-ACP1-MAC") || two zero bytes
|| outer_header || envelope_version || counter || original_packet
```

The key is the sender's direction key. The body length is determined by the
complete datagram length. Both header copies, all original payload bytes and
the version/counter are authenticated. Receivers enforce canonical outer fields,
header agreement, allowed inner types, packet shape and the 1500-byte global
bound before committing replay state. There is no heap allocation in envelope
encoding, decoding or MAC preparation.

## Replay, retries and lifecycle

Each direction has an independent uint64 send counter starting at one, and a
highest received counter plus a 64-bit replay bitmap. Zero is invalid. A packet
at most 63 positions behind the highest counter is accepted once; older packets
and duplicates are rejected indefinitely, without retaining packet histories.
Forged high counters and invalid tags never advance the window. Provider
failures and sender counter exhaustion fail closed, with no bare-control
fallback. The receiver drops a bad packet without terminating the connection.

A UDP retry after EAGAIN retains the exact sealed bytes and counter. A protocol
retry, including KM timer retries, gets a new outer counter. Inner KM counters
still increase only when material changes; retries retain the inner ID/body,
and responses echo that ID. This permits recovery from a lost KMRSP while
rejecting raw recorded envelopes. The KM counter rules separately prevent an
out-of-order old announcement inside the general replay window from rolling
back a DATA key. Neither replay state nor control keys reset on DATA rotation.

NAKs are split to reserve envelope space within the negotiated MSS and a
conservative 48-byte IPv6 IP/UDP allowance. Deferred NAK rearming and queued KM
comparison inspect the original inner body without opening or resealing a
local envelope. The pending FIFO retains its existing bounded capacity.

Runtime handshake retries remain outside the envelope. A connected caller
may resend only its cached AGREEMENT for a matching authenticated response;
ACP1 limits those replies to one per 100 ms and does not let captured handshake
retries refresh peer liveness. Invalid, replayed and bare runtime controls do
not refresh the idle timer. Normal close, queued close drain and the one-shot
idle-expiry SHUTDOWN all use authenticated envelopes.

## Evidence and review still required

Tests cover an independently computed Python hashlib/hmac wire vector, every
byte tampering, reflection/session/profile separation, truncation and packet
bounds, replay-window edges, forged high counters, provider failures, strict
legacy rejection, sync/async sockets, CTR/GCM rotations, deferred UDP bytes,
MSS-bounded sparse NAKs, runtime state/liveness admission and KM retries.
Rendezvous tests cover cookie binding/profile separation, final-proof loss,
bare and signed early-packet rejection, bad proofs, both resolved roles,
IPv4/IPv6, synchronous/asynchronous setup, rotation and unsupported peers.
Group tests cover Broadcast/Backup, CTR/GCM rotation, late joins, replacements,
strict policy, UDP membership/path/standby tampering, member replay isolation
and standby authentication before monitoring mutations. Installed cross-provider
peers additionally cover both group types with all key sizes and replacement.
The optional fuzz target exercises raw and locally signed envelope inputs under
ASan/UBSan. OpenSSL/Mbed TLS cross-provider peers exercise bidirectional rotation
for 16/24/32-byte DATA keys in Caller/Listener and Rendezvous modes. Windows BCrypt qualification remains CI-dependent.

These are implementation checks, not independent cryptographic review. Review
must assess KDF strength/key separation, negotiation/transcript coverage,
replay/loss behavior, lifecycle ordering, coverage of future controls and
resource/performance limits before any supported-profile decision. No upstream
implementation source or private ACP draft was used; the code is independently
authored from the public issue and [RFC 2104](https://www.rfc-editor.org/rfc/rfc2104)
/[RFC 5869](https://www.rfc-editor.org/rfc/rfc5869) primitive contracts.
