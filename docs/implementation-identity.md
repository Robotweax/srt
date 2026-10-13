# Experimental remote implementation identity

The development tree implements an **opt-in, provisional** identity exchange for
[#313](https://github.com/Robotweax/srt/issues/313). Advertisement is off by
default. Released 0.2.9 artifacts do not include it. This is a Robotweax experiment;
the extension identifier has not been agreed with other SRT implementers.

An application can display the peer's implementation name and product release
for diagnostics. These strings are **self-reported claims**. They do not prove
library provenance, authorize a connection or imply any capability. Capability,
crypto and compatibility negotiation remain independent. A peer without valid
identity metadata always has an unknown implementation, regardless of version
numbers or private features it supports.

## Application policy and observation

`SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT` (`0x0100000a`) is an `int32_t` policy:

| Value | Constant | Advertisement |
| --- | --- | --- |
| 0 | `ROBOTWEAX_SRT_IDENTITY_OFF` | No record; default |
| 1 | `ROBOTWEAX_SRT_IDENTITY_NAME` | `Robotweax SRT`, release withheld |
| 2 | `ROBOTWEAX_SRT_IDENTITY_NAME_AND_RELEASE` | Name and loaded library's product release |

Set policy before connect/listen. An accepted socket inherits the listener's
policy; its admission callback can adjust the child's policy before its reply.
The policy is frozen for the subsequent handshake and retries. Arbitrary runtime
identity overrides are unsupported. Application labels belong in an agreed
application channel; the library never rewrites Stream ID. Receive-side
observation works even when local advertisement is off.

```c
int32_t policy = ROBOTWEAX_SRT_IDENTITY_NAME_AND_RELEASE;
if (srt_setsockflag(socket, SRTO_ROBOTWEAX_IDENTITY_ADVERTISEMENT,
        &policy, sizeof(policy)) == SRT_ERROR) {
    /* Older libraries may not implement this experimental option. */
}
```

`SRTO_ROBOTWEAX_PEER_IDENTITY` (`0x0100000b`) returns a copied
`ROBOTWEAX_SRT_PEER_IDENTITY_V1` snapshot. Its 132-byte layout has ABI version,
availability mask, name/release byte lengths, NUL-terminated 33/65-byte arrays,
two explicit padding bytes and four reserved words. ABI version is always 1;
padding/reserved bytes are zero. Lengths exclude terminators. The ABI version is
independent of wire format and product release.

```c
ROBOTWEAX_SRT_PEER_IDENTITY_V1 peer;
int size = sizeof(peer);
if (srt_getsockflag(socket, SRTO_ROBOTWEAX_PEER_IDENTITY, &peer, &size) == 0
    && peer.abi_version == ROBOTWEAX_SRT_PEER_IDENTITY_VERSION
    && (peer.available_fields & ROBOTWEAX_SRT_PEER_IDENTITY_NAME)) {
    /* Display peer.name as self-reported diagnostic metadata. */
    if (peer.available_fields & ROBOTWEAX_SRT_PEER_IDENTITY_RELEASE) {
        /* peer.release is opaque text; never use it to negotiate features. */
    }
} else {
    /* Unknown implementation. */
}
```

A valid socket with no observation returns success, ABI version 1, zero mask,
zero lengths and empty strings. Name-only sets the name bit and leaves release
unavailable. Missing, invalid, duplicate or unsupported metadata has the same
unavailable-value semantics. Short/null buffers fail with `SRT_EINVPARAM` without
partial writes. The peer option is read-only. Both options are socket-only;
group handles return `SRT_EINVOP`, because individual members may differ.
Invalid/closed handles follow normal `SRT_EINVSOCK` rules.

The snapshot is available on the provisional accepted socket during the listen
callback and on each socket after connection completion. Outside admission,
listening sockets and sockets without a completed peer exchange return unknown.
The connected snapshot survives a broken connection until close. Copying it
retains the observation independently of socket lifetime. Rejected admission
clears provisional storage. Late packets cannot rewrite a connected snapshot.

`SRTO_VERSION`, `SRTO_PEERVERSION`, `srt_getversion()` and the local
`SRTO_ROBOTWEAX_VERSION` keep their existing meanings. No existing C layout,
exported function or `SRTO_E_SIZE` value changes.

## Exchange and wire contract

The experiment uses CONFIG extension type **`0x7f13`** with a discriminator and
format version. This value is provisional, not a registered allocation or proof
of a reserved private-use range. Robotweax's other private records occupy
`0x7f10`–`0x7f12`. A shared allocation/convention must be discussed before any
recommendation to enable advertisement by default.

The extension header uses the normal network-order type and four-byte word
count. Version 1 contents are:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Literal ASCII `SRTI` |
| 4 | 1 | Format version, 1 |
| 5 | 1 | Flags, zero |
| 6 | 1 | Name length, 1–32 |
| 7 | 1 | Release length, 0–64 |
| 8 | 4 | Reserved, zero |
| 12 | N | Name bytes |
| 12+N | R | Release bytes |
| 12+N+R | 0–3 | Zero padding to a four-byte boundary |

Text is literal ASCII in wire order, without NUL terminators or Stream ID's
per-word character transformation. Name starts with an alphanumeric byte and
then permits alphanumerics, spaces, `.`, `_`, `+` and `-`, with no trailing space.
Nonempty release starts with an alphanumeric byte and permits alphanumerics,
`.`, `_`, `+` and `-`. Release is opaque text, including prerelease/build labels.
The maximum record is 112 bytes; `Robotweax SRT`/`0.2.9` takes 36 bytes.

Identity is sent only with an existing HSv5 CONCLUSION containing HSREQ or
HSRSP. Caller identity is available to Listener admission; Listener identity
arrives in HSRSP and is available to Caller after connect. In Rendezvous, each
protocol initiator/responder advertises its own label independently of payload
direction. Empty AGREEMENT/DONE packets do not clear identity. No new handshake,
capability flag, echo or bilateral requirement is introduced. Legacy handshakes
carry no identity.

Only one identity record is accepted, in any record order. Wrong magic, unknown
format/flags, reserved fields, invalid text/length/padding or a missing CONFIG
bit makes identity unavailable without rejecting an otherwise valid handshake.
Any duplicate, including identical copies, makes the observation unavailable;
values are never merged and first/last copies are never preferred. Broken outer
TLV framing still has normal packet rejection/retry behavior.

Core records have priority. Identity is omitted when the destination or current
MSS budget cannot hold it (using the existing conservative 48-byte envelope
allowance); enabling diagnostics must not enlarge core limits or fail setup.
No packet-path allocation or RNG is added. Identity storage is fixed-size:
100 bytes per internal value, two values per socket plus the local policy, and
bounded copies in protocol configuration/actions/messages. Payload processing,
crypto and scheduler policies are unaffected. Identity remains visible in the
handshake even when payload encryption is enabled; existing key-material proofs
do not authenticate the library name or release.

## Evidence and qualification boundary

The public [SRT protocol draft](https://haivision.github.io/srt-rfc/draft-sharabayko-srt.html)
describes compatibility version/flags and application Stream IDs; neither is a
general implementation/product-release exchange. MistServer uses Stream ID for
[stream selection](https://docs.mistserver.org/mistserver/outputs/TSSRT/) and
[push tokens](https://docs.mistserver.org/howto/integration/stream_keys/), making
automatic library additions inappropriate.

Local macOS ARM64 evidence uses independently authored public API fixtures and
black-box reference binaries. Reference pins:

- Haivision 1.5.7: `899348d8318eb9a3c5a5b6ec43c4a1114288773a`.
- Frozen AEAD target: `d99d2e1a3b1a213b03c7dfbea5133898935fdeea`.

The initial investigation passed 24 UDP-relay probes (six baselines and eighteen
mutated handshakes) with unfamiliar, future-version and duplicate candidate
records. Subsequent actual advertisement qualification includes IPv4 and IPv6 and uses
`interop/run_identity_interop.py`: all nine policies in Caller/Listener and
Rendezvous, CTR/GCM with all AES key lengths and rotation, and real advertisements
to each reference in both payload directions and endpoint roles. Reference peers
remain unknown because they do not implement this identity convention. These
reference identity cases use plaintext transport, including the AEAD-capable
binary; they are not new GCM interoperability claims.

Unit tests cover malformed content, duplicates/order, byte vectors, bounded
omission, admission/rejection, snapshots, retries, fixed C ABI, deterministic
Rendezvous roles, and encrypted session-authentication confirmation. The handshake
fuzzer exercises the identity content parser directly. Normal CI runs these units
and the self-peer policy matrix; the required Mbed TLS Linux/macOS jobs also run
the real-advertisement harness against both pinned reference builds. Hosted platform/reference qualification and
external feedback remain required before expanding support claims.

Reproduce actual peer checks with a static tools build:

```sh
./tools/python interop/run_identity_interop.py \
  --robotweax-peer /path/to/robotweax_srt_interop_peer --with-aead \
  --reference-peer /path/to/pinned-reference-peer \
  --reference-pin 899348d8318eb9a3c5a5b6ec43c4a1114288773a \
  --output /path/to/fresh-evidence-directory
```

Add `--host ::1` for IPv6 loopback qualification.

Keep advertisement **off by default**. Seek feedback on a reusable convention,
identifier allocation, disclosure policy and deployed version matrix from
MistServer and other implementers. Their feedback has not yet been obtained.
Older versions, actual MistServer builds, independent peers, identifier
collisions and a production deployment are not qualified by these local tests.
