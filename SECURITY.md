# Security policy

## Supported versions

Robotweax SRT is a pre-1.0 project. Security fixes are provided for the current
`main` branch and the latest `0.2.x` release line.

| Version | Security support |
| --- | --- |
| `main` | Active development |
| `0.2.x` | Supported |
| `0.1.x` | Historical; upgrade required |

The supported 0.2.x series consists of development releases. Security support
is not a claim of universal production readiness or drop-in suitability for
every `libsrt` deployment. Evaluate the documented [compatibility](docs/compatibility.md),
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
authenticated. Robotweax therefore rejects malformed, recently retired, inconsistent, or
unwrappable KMREQ/KMRSP input without downgrading an established encrypted
state or enabling clear payload. Authentication failures never publish
provisional plaintext.

Established-session packets must also match the connection's source endpoint.
That check limits off-path injection, but it does not authenticate a packet:
an on-path sender or someone able to spoof the peer endpoint can still submit
runtime controls and AES-CTR DATA. A single packet that violates the negotiated
encryption policy is counted and dropped without acknowledging it or refreshing
peer liveness; it does not terminate the established session. Failure KMRSPs
for rejected KMREQs are limited to one per 100 ms per connection. AES-CTR
still has no payload integrity guarantee; use the opt-in AES-GCM extension
when authenticated DATA is required.

### Optional session binding

The explicitly enabled, default-off `SRTO_ROBOTWEAX_SESSIONAUTH` extension
authenticates session key establishment and runtime KM controls. Both peers
must support it; enabled sessions do not interoperate with ordinary Haivision
peers. See the [wire contract and protection limits](docs/session-authentication.md).
The standard compatible mode remains unchanged. Its bounded receive-key history
cannot reject every captured KMREQ after history eviction. GCM authenticates
DATA but does not provide a lifetime key-epoch replay identity. See the
[bounded-history limitations](docs/encryption.md).

### Encrypted listener admission

Each listener limits encrypted setup attempts with token buckets: a burst of
128 attempts and replenishment of 64 per second overall, and a burst of 32
with replenishment of 16 per second per source IP. Ports, cookies, salts and
IPv4-mapped address spellings share the source budget. Failed attempts and
retries consume it too. Each admitted setup can derive at most one receive
and one transmit key-encryption key, plus one session-authentication key
when the optional protection is enabled. The source table has 128 fixed entries
and only reuses entries whose full burst has replenished.

Excess conclusions are dropped before key derivation and normally before
accepted-socket allocation. If a listen callback enables encryption, the same
budget is checked after that callback and before crypto construction. Peers
can retry within their connection timeout; clients sharing a NAT also share
the source budget. Existing established connections and plaintext admissions
are unaffected. These bounds contain listener-local derivation work; they do
not guarantee admission during a distributed flood or impose a process-wide
limit across multiple listeners.

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
