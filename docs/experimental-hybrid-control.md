# Experimental hybrid control components

These are staged components behind
`ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_HYBRID_CONTROL=ON`, default **OFF**. The build
also requires `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_ACP1=ON` explicitly. They provide
strict capability/message codecs and an authenticated, bounded message gate.
They also include a setup-only admission owner for explicit pairwise credentials,
frozen conclusion transcripts and final local authorization checks.
They perform no socket I/O, change no delivery policy, allocate no public C
option, and do not activate recovery on live sockets. Native handshakes still
reject authentication version 7. No new capability handshake extension ID is
allocated. Existing strict multicast announcements and existing authentication
versions retain their formats.

```sh
cmake -S . -B build-hybrid-control \
  -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_ACP1=ON \
  -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_HYBRID_CONTROL=ON
cmake --build build-hybrid-control --parallel
./build-hybrid-control/robotweax_srt_tests --include hybrid_control
./build-hybrid-control/robotweax_srt_tests --include hybrid_admission
./tools/python tests/hybrid_control_vectors.py
```

## Capability record

The provisional body version is 1; all integers are network byte order. Encoders
require the exact destination size and leave it unchanged on error. Decoders
reject incorrect lengths, versions, reserved bytes and invalid fields.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Version, 1 |
| 4 | 4 | Kind: offer 1, selection 2 |
| 8 | 4 | Supported/selected feature bits |
| 12 | 4 | Required feature bits |
| 16 | 4 | Maximum prefix, 1 through 256 packets |
| 20 | 4 | Reserved, zero |
| 24 | 8 | Stream incarnation |
| 32 | 8 | Member incarnation |
| 40 | 8 | Initial absolute sequence position |

Bits 1, 2 and 4 mean canonical unicast originals, bounded prefix recovery and
progress reports. All three are mandatory. Bits 8 and 16 mean overlap and source
recovery. Unknown bits are rejected, required bits must be supported, and a
selection must satisfy both peers' requirements. The prefix limit is the smaller
of their limits. Offers have zero incarnation/position fields. A selection has
nonzero stream and member incarnations; position may be zero. A receiver checks
that the selection is a subset of its offer. Failure is explicit; it does not
silently reduce the required profile.

## Authenticated context

The canonical 192-byte context is:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic/version `0x52574801` |
| 4 | 60 | Existing multicast dispatch announcement content |
| 64 | 32 | Existing fixed-source announcement content |
| 96 | 48 | Complete receiver offer |
| 144 | 48 | Complete sender selection |

The dispatch/source group IDs and families must match. The source must be an
announcement with a valid fixed unicast address/port. The legacy encodings remain
unchanged. Lossy local-only address fields are refused rather than omitted from
the authenticated identity. IPv6 scope handling remains local; this codec is not
native IPv6 hybrid qualification.

The gated, component-only authentication version 7 uses the existing
[experimental ACP1](experimental-acp1.md) primitives. Its root salt is `ASCII("RWSRT-SESSION-7") || NUL` (16 bytes), caller and listener nonces
(32 bytes each), caller and listener socket IDs (uint64 each), then the complete
192-byte context. The root derivation remains PBKDF2-HMAC-SHA1 with 2048
iterations and a 32-byte result. Directional key expansion and the ACP1 control
MAC construction are unchanged; the version-specific root separates this profile
from versions 2 through 6. Rendezvous cookies are refused.

The owner must provision a **separate pairwise control credential** to these
objects. The components never read shared DATA keys; the owner must not supply
traffic key material as the credential. Reusing a group-wide passphrase does
not provide member isolation: anyone holding the control credential can
impersonate its peers. There is no forward secrecy, protocol certification or
hostile-network support claim. The existing ACP1 KDF/key-separation review limits
still apply. The component API cannot prove how an application provisioned its
credential.

Both peers must verify the opposite role's handshake proof, bound to the complete
initial key material, before controls can be used. A self-proof cannot confirm
the peer. Exactly one message gate may claim a confirmed authentication object;
closing the gate cannot reset the incarnation or replay state. New incarnation
fields require a fresh authentication context and fresh peer setup.

Version 7 accepts only the provisional inner recovery-control subtype `0x7f16`,
inside the existing `0x7f05` ACP1 envelope. Local namespace review found no prior
use of `0x7f16`; it is not a registered or stable wire assignment. Existing ACP1
versions reject that subtype. Version 7 refuses ordinary controls, KM and FEC;
it cannot substitute for those profiles. Outer and inner socket identity,
timestamp, direction, tag and the existing 64-packet cryptographic replay window
remain checked. No production handshake decoder admits version 7. This context does not
authenticate every handshake field: for example, Stream ID, caller group
metadata and transport limits are not a complete authenticated transcript.
It must not be treated as a complete-handshake proof. Live integration requires
a separate credential-provisioning and final admission/transcript review.

## Credential and admission owner

`compat::hybrid::Admission` is the staged owner of a separate version 7
authentication object. It is serialized by its caller, performs no I/O or
credential lookup, and has no timer. Its bounded storage is under 8 KiB. It
does not replace the ordinary session authentication object: version 7 refuses
ordinary controls, KM and FEC, so live integration needs two distinct owners.
The ordinary handshake/authentication and address/cookie admission checks must
already have succeeded before this owner can authorize hybrid controls.

The application supplies an explicit local `Authorization`: a nonzero 16-byte
credential ID, exact caller and listener group IDs, exact Stream ID, nonzero
policy generation and an exclusive monotonic expiry. An empty Stream ID is an
explicit empty-string grant, not a wildcard. The credential ID is a lookup
identifier, not a secret or proof of identity. There is no default credential,
automatic lookup, socket passphrase inheritance, shared DATA key input or
fallback after an absent/invalid credential. `establish` requires the matching
ID and an independently provisioned, nonzero 32-byte pairwise random secret.
The API validates its size, not its entropy or provisioning history. Both peers
must receive matching secret material through a separately trusted mechanism;
that mechanism and a public configuration interface remain unimplemented.

The secret is used synchronously for the existing version 7 root derivation
and is never retained by the admission owner. The supplier owns its original
buffer and must erase it when appropriate. Root and directional keys are erased
on terminal revocation and destruction. A revoked root cannot be re-established;
an already borrowed control gate immediately becomes unusable. A fresh owner,
fresh nonces and a fresh admission decision are required to try again.

`prepare` copies both final, exact HSV5 CONCLUSION **payloads**, excluding the
outer 16-byte control header, into one immutable transcript. It validates caller
request/listener response roles, nonzero distinct socket IDs, both multicast
group memberships, the explicitly authorized Stream ID and the selected
dispatch/source announcements. Both sequence origins must equal the advertised
dispatch ISN. This initial owner excludes Rendezvous, packet filters, standby
measurement and unknown extensions. It accepts only the Live controller.
Existing ordinary authentication records can be present and are included
unchanged; native decoding still rejects a version 7 record.

The transcript binds all bytes of both payloads, including core transport
limits, cookies, peer-address fields, handshake flags/latencies, caller group,
Stream ID, path identifiers, ordinary authentication/KM and any accepted suffix
or diagnostic bytes. It does not re-encode decoded fields or discard fields
ignored by older parsers. Reordered or changed payload bytes require a new
admission attempt; only the outer packet timestamp may change on retransmission.
The owner must supply the actual sent/received payloads, rather than reconstruct
them from partial socket metadata. This is a binding of the two conclusions,
not of the induction exchange, routing provenance or the complete handshake.

| Offset | Bytes | Admission proof material |
| --- | --- | --- |
| 0 | 4 | Magic/version `0x52574101` |
| 4 | 4 | Total material length |
| 8 | 4 | Caller payload length |
| 12 | 4 | Listener payload length |
| 16 | 16 | Credential ID |
| 32 | 192 | Existing canonical hybrid context |
| 224 | variable | Exact caller payload, then exact listener payload |

Each payload is at most 1,484 bytes; proof material is at most 3,192 bytes.
The limit bounds startup storage and is not an MTU qualification. The two
opposite-role proofs use this whole material with the existing version 7 proof
domain and root; no new digest/KDF or wire extension is introduced. The low-level
version 7 proof primitive now accepts this bounded length; versions 2 through 6
retain their 128-byte limit and original bytes. Independent Python hashlib/hmac
vectors cover both the prior primitive and the complete admission fixture.

`authentication()` releases a borrowed root only after a valid opposite-role
proof and a final `confirm` check against the current local grant. The local
policy generation and expiry are not wire assertions; each endpoint has its
own authorization decision. Any changed grant (including renewal), expiry,
clock regression, invalid credential, establishment failure or invalid peer
proof closes the owner. A transient local proof-generation provider error can
be retried without publishing partial output. After confirmation the runtime
owner must call `revalidate` on policy changes and at expiry, serialized with
control handling; this class does not schedule those callbacks. Revocation
invalidates borrowed gates, but gate closure or an expired deadline cannot undo
already authorized coordinator work. The future bridge must fence that work
at its commit boundary. All borrowed gates must die before this owner.

Live admission must atomically recheck the grant generation with current group
incarnation, binding, source state, exact sequence cursor and selected features
after user callbacks and before binding publication. A nonzero generation is
only a token here: this component has no policy store and cannot discover
revocation or prove that a callback actually ran. No native path invokes this
owner yet. The next integration must define the separate credential-provider
contract, capability/proof carrier, final enrollment transaction and expiry
scheduling before enabling runtime switching. Existing ACP1 security-review
limits and the shared-credential impersonation boundary remain unchanged.

## Control body

Every control body is exactly 96 bytes.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Version, 1 |
| 4 | 4 | Kind, 1 through 6 |
| 8 | 8 | Stream incarnation |
| 16 | 8 | Member incarnation |
| 24 | 8 | Nonzero semantic serial |
| 32 | 8 | Policy revision |
| 40 | 8 | Next absolute sequence position |
| 48 | 4 | Matching 31-bit next sequence |
| 52 | 4 | Mode: multicast 0, unicast 1, overlap 2 |
| 56 | 8 | Retained floor |
| 64 | 8 | Multicast next position |
| 72 | 8 | Produced next position |
| 80 | 4 | Activity: startup, producing, paused, ended, source failed (0–4) |
| 84 | 4 | Outcome: OK, denied, history unavailable, budget, stale, busy, unsupported (0–6) |
| 88 | 4 | Observation/report interval in microseconds |
| 92 | 4 | Report age in microseconds |

Sender kinds are STREAM_PROGRESS (1), DELIVERY_COMMIT (4), DELIVERY_STATUS (5).
Receiver kinds are PATH_REPORT (2), FALLBACK_REQUEST (3), MULTICAST_READY (6).
The gate checks roles, incarnation equality and the sequence mapping from the
selected initial absolute position and advertised ISN, including multiple wire
wraps. It does not infer positions from wire sequences alone.

Progress and path reports carry a positive interval no larger than 1,000,000
microseconds and age no larger than 5,000,000 microseconds. These are parsing
bounds, not production detection thresholds. Fields unused by a kind must be
zero. Only status may report a non-OK outcome. Only progress may report source
activity. Requests select unicast; commits have a nonzero revision. Overlap and
ready messages require negotiated overlap support.

STREAM_PROGRESS distinguishes production from actual original-submission high
water and multicast-submission high water. A high water mark is not proof of a
contiguous submitted prefix or an ACK horizon. Retention can advance beyond an
older emitted position. A future producer of these records must use actual UDP
outcomes; codec validity alone cannot establish truth or delivery. PATH_REPORT
separates total validated DATA progress from multicast observations, including
valid multicast duplicates already delivered by unicast.

## Bounded admission and retry semantics

The message gate stores 64 fixed-size semantic records in each direction, in
addition to the authentication replay window. Matching serial/body retries use a
fresh authenticated envelope and return duplicate; changed content under the
same serial returns conflict. Older records outside the window are stale. Serial
exhaustion cannot wrap to zero. A later progress report prevents an older report
from replacing it, even inside the cryptographic reorder window.

A duplicate report retains its first receive time and cannot refresh evidence.
Freshness accounts for both authenticated advertised age and elapsed local
monotonic time. Clock regression, failed authentication, conflicting content and
wrong-role/incarnation messages do not publish a new report. Provider failure or
a short output does not consume a send serial. Gate storage is bounded to 16 KiB and its bookkeeping performs no dynamic
allocation; the supplied cryptographic provider retains its own allocation behavior.

An authenticated accepted event is **not permission to mutate policy**. This
stage has no coordinator adapter, retry scheduler, failure detector or public
socket option. The future owner must validate current policy revision, retained
history, deadlines and budgets before invoking a coordinator transaction. Peer
claims can still be false; authenticated traffic alone is not source DATA,
source liveness or multicast-failure evidence.
