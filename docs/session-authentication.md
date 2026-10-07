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

This does not authenticate every SRT control or handshake field. In particular,
stream IDs, transport settings and ordinary ACK/NAK/shutdown traffic are not
covered. AES-CTR still lacks DATA integrity; select the separate GCM mode when
payload authentication is required. Shared-passphrase holders can impersonate
one another. There is no forward secrecy. Use high-entropy passphrases; the
2048-iteration password KDF is not a password-storage recommendation. Admission
budgets still apply, with one additional session KDF per protected setup.

## Version 1 wire contract

All integers are network byte order. INDUCTION is unchanged. CONCLUSION uses
the configuration-extension bit and private extension type `0x7f10`, with a
100-byte content: version `uint32(1)`, caller nonce (32 bytes), listener nonce
(32 bytes), and HMAC-SHA256 proof (32 bytes). The caller's first offer has a
fresh caller nonce and zero listener nonce/proof. The listener responds with
its fresh nonce and server proof. The caller verifies it and sends AGREEMENT
with the caller proof. The listener retries its response until confirmation;
the connected caller can resend its cached AGREEMENT upon a valid response.
Unknown versions, malformed sizes and duplicate extensions are rejected.

The 32-byte authentication key is PBKDF2-HMAC-SHA1(passphrase, salt, 2048).
The salt is the 16 bytes `RWSRT-SESSION-1` followed by NUL, both 32-byte nonces
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
