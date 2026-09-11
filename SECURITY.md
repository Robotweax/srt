# Security policy

## Supported versions

Robotweax SRT is a pre-1.0 project. Security fixes are provided for the current
`main` branch and the latest `0.2.x` release line.

| Version | Security support |
| --- | --- |
| `main` | Active development |
| `0.2.x` | Supported |
| `0.1.x` | Historical; upgrade required |

Version 0.2.4 is an evidence-backed development release, not a claim of
universal production readiness or drop-in suitability for every `libsrt`
deployment. Evaluate the documented [compatibility](docs/compatibility.md),
[limitations](docs/limitations.md), and deployment threat model before use.

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability.

Email suspected vulnerabilities to Robotweax GmbH at
[info@robotweax.ai](mailto:info@robotweax.ai), with the subject
`Robotweax SRT security report`. This contact is available to new reporters;
no prior relationship with the maintainers is required.

Alternatively, use GitHub's **Report a vulnerability** button on the
repository's [Security advisories page](https://github.com/Robotweax/srt/security/advisories)
when private vulnerability reporting is enabled. If that option is unavailable,
use the email address above rather than a public issue or discussion.

Include:

- the affected Robotweax version or commit and platform;
- the smallest reproducible input or scenario;
- expected and observed behavior;
- security impact and required preconditions;
- relevant sanitizer output or packet evidence with secrets and payloads
  removed.

Never send production passphrases, private keys, credentials, unredacted
packet captures, or confidential application data.

The maintainers will acknowledge the report, reproduce it, determine affected
versions, and coordinate a fix and disclosure. No fixed response-time or
embargo guarantee is offered for this pre-1.0 release line.

## Security model

Security-relevant components include packet and extension decoders, handshake
state machines, cryptographic negotiation and key-material processing,
sequence arithmetic, replay and retransmission state, bounded queues and
buffers, compatible public APIs, file adapters, and lifecycle cleanup.

### AES-CTR

The default SRT security mode uses the protocol's compatible AES-CTR/HaiCrypt
profile. It provides payload confidentiality but not authenticated payload
integrity. PBKDF2 parameters are implemented for wire compatibility and are
not a modern password-storage recommendation.

### AES-GCM extension

Robotweax SRT 0.2.0 also provides a version-pinned authenticated AES-GCM
extension. It is enabled at build time with the historical flag
`ENABLE_AEAD_API_PREVIEW=ON`; the extension itself is released, opt-in, and
default-off. Both the library and consumer must use the matching public header
contract. Explicit GCM negotiation fails closed and never silently falls back
to CTR.

The supported algorithms, wire contract, interoperability boundary, and
deployment limits are documented in [Encryption and key rotation](docs/encryption.md)
and the [AES-GCM extension contract](docs/aes-gcm-contract.md).

### Runtime key material

The compatible SRT key-material control format is not independently
authenticated. Robotweax therefore rejects malformed, stale, inconsistent, or
unwrappable KMREQ/KMRSP input without downgrading an established encrypted
state or enabling clear payload. Authentication failures never publish
provisional plaintext.

## Secure integration guidance

- Use unique, high-entropy passphrases delivered through a secure secret
  channel.
- Set encryption requirements before connecting or listening.
- Treat compatibility downgrade, authentication failure, peer error, and
  negotiation rejection as terminal unless the application deliberately
  starts a new connection.
- Do not log passphrases, derived keys, key-encryption keys, unwrapped session
  keys, plaintext, or full sensitive packets.
- Keep OpenSSL and the host operating system within their supported security
  lifecycles.
- Validate resource, latency, and failure behavior for the intended topology
  before production deployment.
