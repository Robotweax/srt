# Robotweax SRT 0.2.3 release notes

Release date: **2026-09-07**

Project version: **0.2.3**

Shared-library ABI line: **0.2**

Default Haivision-compatible API profile: **SRT 1.5.7**

This patch release collects encryption, socket-lifecycle, handshake, group
timing, packaging and validation corrections since 0.2.2. It does not add a
public API, change the 80-symbol C export surface, or enable AES-GCM by default.
The project version is independent of `srt_getversion()`, which remains 1.5.7.

This is a pre-1.0 release with explicitly retained qualification limits,
not a claim of universal drop-in compatibility or independent security audit.

## Encryption and upgrade behavior

After an encrypted handshake, each Robotweax endpoint confirms independent
local transmit key/salt material through the existing runtime KMREQ/KMRSP
exchange before releasing DATA. Handshake material may still be retained for
legacy peer reception, but is not reused as a bidirectional Robotweax DATA key.
Cipher suites, key lengths, key wrapping, DATA format and public options stay
unchanged. Initial confirmation may add a control exchange and expose
`SRTO_SNDKMSTATE=SECURING` for longer during connection startup.

Failed local preparation or key-control errors after encrypted negotiation
cannot silently downgrade queued DATA to plaintext. Initial optional-encryption
negotiation retains its documented behavior. Native key refresh intervals are
also bounded at `2^31 - 1` packets before accepting configuration changes.

Rebuild against the corrected library and qualify startup deadlines with the
actual peer and encryption profile. See [Encryption](encryption.md) and the
[AES-GCM contract](aes-gcm-contract.md). These implementation corrections do
not establish new upstream cipher support or claim authentication of every
SRT control packet, forward secrecy or certified peer identity.

## Runtime and public examples

- Closed socket records are reclaimed and their configured secrets erased;
  resources no longer accumulate solely with the number of historical sockets.
- Delayed INDUCTION replies cannot restart an already advanced caller
  handshake exchange.
- Group members share timestamp origin and receive TSBPD state, including
  late joins and rollover replay. Received messages retain the deadline
  snapshot that authorized their release.
- Message, File and Group demos honor explicit CTR/GCM selections instead of
  leaving an explicit CTR request in AUTO mode.

Update both Robotweax group endpoints for synchronized timing; mixed old/new
group-clock behavior is not qualified. Native source-tree C++ consumers must
rebuild because internal layouts changed; they are not the stable C ABI.

## Integration and validation improvements

- Static pkg-config C consumers compile with a C compiler and link with a C++
  linker; shared C integration retains its existing recipe.
- ACKACK/PEERERROR documentation matches the existing four-byte zero-word CIF.
- FEC documentation distinguishes physical packet/byte traffic from recovered
  unique data and does not imply separate public FEC-byte counters.
- Reference provenance distinguishes primary SRT 1.5.7 from legacy, security
  and pinned AEAD profiles.
- The required CI gate includes FFmpeg; UBSan findings are fatal and checked
  by an instrumentation canary.
- Failed AEAD timing checks can retain allowlisted diagnostic JSON and logs
  in short-lived artifacts. Raw payloads, process output and reference
  binaries are not part of those diagnostic uploads.
- Robotweax CI tool archives include license/third-party notices; reference
  binaries are runner-local and are not distributed as release artifacts.
- A private email reporting fallback is documented in [Security policy](../SECURITY.md).

## Qualification limits and release acceptance

The supported API, ABI, cipher and topology boundaries remain those in
[Compatibility](compatibility.md) and [Known limitations](limitations.md).
AES-GCM remains an explicit default-off extension. Upstream GCM File/Stream
support, positive HSv4 establishment and arbitrary libsrt ABI substitution
are not claimed.

Intermittent Backup-group interoperability failures and host-dependent timing
outliers remain under qualification. A successful retry does not establish
their cause or a universal failover guarantee. Validate the exact peer,
traffic pattern and failover topology before production group deployment;
do not infer hard-real-time guarantees from loopback or shared-runner results.

The maintainer has accepted release of 0.2.3 with these documented limitations.
An independent cryptographic review of the directional-key changes has not
been completed. Successful platform, package, sanitizer, interoperability and
FFmpeg checks do not substitute for that review or establish the cause of
intermittent group failures. This release is not an independent security
certification or a general production-readiness guarantee.

## Distribution

The immutable `v0.2.3` tag identifies the release source. GitHub's tag source
archives include the MIT and third-party notices; Haivision binaries are not
redistributed. Existing tags, including `v0.2.2`, retain their original
contents. Public repository migration and history handling are separate
from this release.
