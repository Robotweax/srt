# Optional session authentication

`SRTO_ROBOTWEAX_SESSIONAUTH` is a Robotweax-specific boolean socket option,
**false by default**. Enable it explicitly on both Caller and Listener before
connection setup. It requires a passphrase and enforced encryption. Setting
it never implicitly enables GCM. Rendezvous is not supported with this option;
the conflicting setting is rejected in either order.

```c
bool enabled = true;
if (srt_setsockflag(socket, SRTO_ROBOTWEAX_SESSIONAUTH,
                   &enabled, sizeof(enabled)) == SRT_ERROR) {
    /* Handle unsupported/conflicting configuration. */
}
/* Configure SRTO_PASSPHRASE securely before connect/listen. */
```

The option is inherited by accepted sockets. A peer without matching support,
a mismatched passphrase, or missing authentication causes setup to fail. There
is no automatic fallback. This private extension is not Haivision-compatible
when enabled. With the option disabled, ordinary SRT handshake and key-control
formats remain unchanged and no session-authentication KDF is performed.

## Protection and limits

The extension binds initial key material and subsequent KMREQ/KMRSP messages
to fresh nonces from both peers, both socket identities and their roles. The
listener does not publish an accepted connection until it has verified the
caller's final proof. Captured messages from a different session cannot install
keys in a new protected session. Direction-separated counters reject older
runtime key controls while permitting identical loss-recovery retries.
Version 2 also authenticates FEC parity headers and payloads before decoder
admission, with an independent replay window in each direction.

This does not authenticate every SRT control or handshake field. In particular,
stream IDs, transport settings and ordinary ACK/NAK/shutdown traffic are not
covered. AES-CTR still lacks DATA integrity; select the separate GCM mode when
payload authentication is required. Shared-passphrase holders can impersonate
one another. There is no forward secrecy. Use high-entropy passphrases; the
2048-iteration password KDF is not a password-storage recommendation. Admission
budgets still apply, with one additional session KDF per protected setup.

## Version 2 wire contract

Version 2 requires updated peers on both sides. Version 1 is rejected during
setup; there is no downgrade or mixed-version fallback. This change affects
only explicitly enabled session authentication. Ordinary Haivision-compatible
connections still use the unchanged standard wire formats.

All integers are network byte order. INDUCTION is unchanged. CONCLUSION uses
the configuration-extension bit and private extension type `0x7f10`, with a
100-byte content: version `uint32(2)`, caller nonce (32 bytes), listener nonce
(32 bytes), and HMAC-SHA256 proof (32 bytes). The caller's first offer has a
fresh caller nonce and zero listener nonce/proof. The listener responds with
its fresh nonce and server proof. The caller verifies it and sends AGREEMENT
with the caller proof. The listener retries its response until confirmation;
the connected caller can resend its cached AGREEMENT upon a valid response.
Unknown versions, malformed sizes and duplicate extensions are rejected.

The 32-byte authentication key is PBKDF2-HMAC-SHA1(passphrase, salt, 2048).
The salt is the 16 bytes `RWSRT-SESSION-2` followed by NUL, both 32-byte nonces
(caller first), then caller and listener socket IDs, each encoded as uint64.
HMAC input is that same 16-byte label, one domain byte (1 handshake,
2 KMREQ, 3 KMRSP), one sender-role byte (1 caller, 2 listener), a uint64
counter (zero for handshake), uint16 body length, and the complete KM body.
Handshake proofs cover the original initial KM body, including wrapped keys.

Runtime private control subtypes are `0x7f03` (KMREQ) and `0x7f04` (KMRSP).
Their payload is uint64 counter, unchanged KM body, and 32-byte tag. Request
counters start at one and increase when the body changes; retries reuse the
counter. Responses echo the authenticated request counter. The receiver checks
the MAC and counter before key processing. Counter exhaustion fails closed.
Bare standard KMREQ/KMRSP controls are ignored in protected sessions; protected
subtypes are ignored in ordinary sessions. Replay storage is fixed-size.

The HMAC construction follows [RFC 2104](https://www.rfc-editor.org/rfc/rfc2104);
provider tests include [RFC 4231](https://www.rfc-editor.org/rfc/rfc4231) vectors.
The implementation is independently authored. Native Windows BCrypt behavior
requires the Windows CI profile; local POSIX tests exercise OpenSSL.

## Authenticated FEC parity

When session authentication and a packet filter are enabled, each parity DATA
packet (message number zero) carries uint64 counter, the unchanged FEC body,
and a 32-byte HMAC-SHA256 tag. Counters start at one and increase for each
emitted parity packet, independently of key-control counters and key rotations.
HMAC input is the version-2 label, domain byte 4, sender-role byte, uint64
counter, uint16 FEC-body length, the complete 16-byte DATA header in network
order, and the FEC body. Every header bit, including retransmission, is covered.
A queued local datagram retry retains the exact counter and serialized bytes.

The receiver verifies the MAC before changing replay or FEC state. A 64-packet
sliding bitmap accepts each counter once, tolerates bounded reordering, and
rejects older packets without an ever-growing history. Greater reordering can
lose a repair opportunity; normal ARQ remains available when configured.
Bare, modified, reflected and foreign-session parity fails closed. Counter
exhaustion or a signing-provider failure terminates the sending connection.
The 40-byte envelope is reserved in the MSS/IPv4/IPv6 payload budget only when
both session authentication and FEC are enabled. Received parity is still not
used as independent evidence of source-DATA liveness.

This authenticates parity, not AES-CTR source DATA. Use AES-GCM as well when
source integrity is required; CTR's unauthenticated DATA remains malleable.
Without session authentication, compatible FEC parity remains unauthenticated.
