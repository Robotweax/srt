# Experimental hybrid control components

These are staged components behind
`ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_HYBRID_CONTROL=ON`, default **OFF**. The build
also requires `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_ACP1=ON` explicitly. They provide
strict capability/message codecs and an authenticated, bounded message gate.
They also include a setup-only admission owner for explicit pairwise credentials,
frozen conclusion transcripts and final local authorization checks.
The exchange drives bilateral selection and proofs with bounded retries
and an explicit credential-provider contract. An explicitly attached internal
native service now owns that exchange, authenticated UDP controls and expiry.
An explicitly attached sender-side coordinator bridge can now grant bounded
unicast fallback commands. An explicitly attached receiver owner now tracks
revisions and reconciles policy with bounded read-only queries. These components allocate no public C
option and do not activate automatic recovery on live sockets. Native handshakes still
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
./build-hybrid-control/robotweax_srt_tests --include hybrid_exchange
./build-hybrid-control/robotweax_srt_tests --include hybrid_native
./build-hybrid-control/robotweax_srt_tests --include hybrid_bridge
./build-hybrid-control/robotweax_srt_tests --include hybrid_receiver
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
recovery. Bit 32 selects read-only policy queries and request-correlated delivery
responses. Unknown bits are rejected, required bits must be supported, and a
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

`prepare` copies the selected, exact HSV5 HSREQ and HSRSP CONCLUSION **payloads**, excluding the
outer 16-byte control header, into one immutable transcript. It validates caller
request/listener response roles, nonzero distinct socket IDs, both multicast
group memberships, the explicitly authorized Stream ID and the selected
dispatch/source announcements. Ordinary handshake sequence proposals are bound
as separate fields; the dispatch announcement alone defines the canonical
multicast DATA sequence origin. They need not be equal. This initial owner excludes Rendezvous, packet filters, standby
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
revocation or prove that a callback actually ran. The internal native service
below invokes this owner through Exchange and owns credential callbacks and
expiry. The explicit bridge below adds a local authorization lease and final
coordinator transaction. Public provisioning, carrier registry review, detection
and policy reconciliation remain prerequisites for automatic switching.
Existing ACP1 security-review
limits and the shared-credential impersonation boundary remain unchanged.

## Native conclusion capture

With the hybrid build gate enabled, native independent-source Caller/Listener
setup now retains an immutable `Conclusions` pair on each established member
runtime. Capture alone does not activate hybrid control; an internal consumer
must explicitly attach the native service below. Other profiles retain no
published pair. Capture introduces no public C option or delivery-policy change.

The native writer captures the exact encoded payload only after full UDP
submission. Partial submission invalidates that setup's capture. The listener
inbox carries the original datagram through admission and setup handoff rather
than reconstructing it from decoded fields. Source/dispatch records are also
preserved by the decoded-only handoff fallback. Blocking and asynchronous
Callers freeze the actual received HSRSP together with the submitted HSREQ.
The Listener freezes the admitted original HSREQ together with its submitted
HSRSP, after setup, runtime attachment, native group enrollment and successful
response I/O. Compact authenticated AGREEMENT records do not replace either
conclusion. Existing primary authentication, when configured, still must succeed
through its normal state machine before publication.

The pair preserves every payload byte, including peer-address fields, KM,
ordinary authentication records, diagnostic metadata and accepted ignored
extension suffixes. It excludes the outer control header, whose packet timestamp
may change on retransmission. Frozen snapshots cannot be changed by later
capture or replay. Publication is single assignment under the runtime mutex,
requires matching local/peer socket identities and refuses closed or broken
runtimes. The getter hides the pair after close/breakage. A retained snapshot
is immutable historical metadata, not a live authorization or binding lease.

Native HSV5 responses may echo the Caller's ordinary ISN while independently
advertising the multicast DATA cursor. The admission validator now accepts
these distinct origins, retaining them in the authenticated transcript; it still
checks the exact source/dispatch announcement. Hybrid DATA position mapping
continues using the dispatch ISN. Equating these origins would reject legitimate
native connections and late joins.

The capture builder and frozen pair are each under 4 KiB, with 1,484 bytes per
payload. A feature-enabled listener inbox reserves an extra 1,500-byte raw
datagram field per existing bounded slot. A frozen pair is allocated only for
successfully captured independent-source setup. The default build has neither
the raw inbox field nor the runtime snapshot slot; no queue, scheduler service
or worker is added. Native consumers must wait for pair publication rather than
infer readiness from a group row's CONNECTED observation during setup.

Native loopback tests compare both endpoints' complete captured payloads and
feed them directly into the staged exchange with explicit synthetic credentials.
They cover plaintext/CTR/GCM, blocking/asynchronous Callers, ordinary session
authentication's final AGREEMENT and existing join/departure/key lifecycles.
Those capture tests exercise an in-memory exchange. Separate native-service
tests below exercise the UDP carrier and credential/expiry ownership. The
explicit bridge below adds atomic coordinator enrollment for its bounded scope.

## Bounded exchange and credential provider

`compat::hybrid::Exchange` drives four staged records after the ordinary
connection's conclusions are frozen. It performs no socket I/O, enrollment or
coordinator mutation. Its storage is bounded to 16 KiB and its bookkeeping
allocates no dynamic memory. The native service below provides an internal
carrier; no public credential API or registered stable wire extension exists.
Native handshakes continue to refuse authentication version 7.

| Direction | Record | Content and effect |
| --- | --- | --- |
| Caller to Listener | OFFER, 1 | Caller nonce and complete capability offer |
| Listener to Caller | SELECTION, 2 | Bilateral selection, both nonces and Listener conclusion-transcript proof |
| Caller to Listener | PROOF, 3 | Both nonces and Caller conclusion-transcript proof |
| Listener to Caller | RECEIPT, 4 | Both nonces and Listener receipt for the verified Caller proof |

Each record has exactly 176 bytes, with network-order integers:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic/version `0x52575801` |
| 4 | 4 | Record kind, 1 through 4 |
| 8 | 4 | Caller socket ID |
| 12 | 4 | Listener socket ID |
| 16 | 16 | Explicit credential ID |
| 32 | 48 | Capability offer/selection, zero for PROOF and RECEIPT |
| 80 | 32 | Caller nonce |
| 112 | 32 | Listener nonce, zero for OFFER |
| 144 | 32 | Proof, zero for OFFER |

Socket IDs must be distinct and nonzero. The credential ID and Caller nonce
must be nonzero. All later records require both nonces and a nonzero proof.
Authentication version 7 is implied by this body format. Unused capability
bytes must be zero. Exact-size codecs validate before writing destinations.
The authenticated context is constructed from the actual offer and selection
plus the previously frozen dispatch/source announcements; these announcements
are not inferred from the exchange sender address. Only the Listener supplies
the stream/member incarnations and initial absolute position.

The Caller generates its offer nonce once with `Admission::begin`, then freezes
the final context after receiving a valid selection. The Listener verifies
both sides' required features and selects their supported intersection and
minimum prefix limit. Each endpoint resolves its credential once, derives one
root and verifies its peer's conclusion proof. The Caller withholds its root
from the runtime until it also verifies RECEIPT. The Listener can expose its
root after verifying PROOF and constructing RECEIPT. Root exposure is local
authentication readiness, not proof that both runtimes are installed, DATA is
flowing or any delivery policy has been granted.

Receipt proof material is exactly `ASCII("RWSRT-HYB-READY") || NUL` (16 bytes)
followed by the 32-byte verified Caller conclusion proof. It uses the existing
version 7 Listener role-proof primitive, with no new KDF or MAC construction.
This material's prefix is disjoint from the canonical conclusion transcript's
magic, so an ordinary Listener conclusion proof cannot serve as a receipt.
The root already binds both nonces, socket IDs and the selected context; the
included Caller proof binds both complete conclusion payloads. No runtime
control counter or semantic serial is consumed. Independent Python vectors
cover all four exact records and this receipt proof.

`CredentialProvider` has two borrowed, `noexcept` callbacks. `resolve` receives
the frozen request and fills a local grant plus exactly 32 secret bytes. Its
secret output is erased through the crypto provider on every return path,
including denial, malformed output, crypto failure and reentrant close.
`revalidate` receives the same request and expected grant and must return the
current local grant. Denial or any change closes the attempt; renewal requires
a fresh owner. There is no retry or alternate credential lookup after denial.

The request carries the locally configured established peer, connection
generation, endpoint role, socket IDs, exact caller/listener groups and Stream
ID, explicit credential ID and selected hybrid context. The peer address and
connection generation come only from the native owner configuration, never the
record. Before lookup, the component validates its bounded frozen baseline
payloads. The native owner must still establish routing/cookie/session
provenance and ensure that the callback's policy store authorizes that identity;
the component cannot prove these facts from a nonzero generation. Callbacks
must run without group/runtime locks, be nonblocking and outlive the exchange.
Close from a callback is terminal and prevents output/root publication. The
native service owns callback context and dispatch; public configuration remains
future integration.

`start`, `receive` and `tick` are serialized and take monotonic microseconds.
Configuration bounds setup lifetime to 1 through 5,000,000 microseconds, retry
interval to 1 through that lifetime and emissions to 1 through 8 per outgoing
stage. Defaults are a one-second lifetime, 100 ms retry interval and four
emissions; they are experimental component settings, not WAN continuity claims.
Each role has two outgoing stages, so at most 16 records can be published per
endpoint/attempt. An output counts against that bound even if its native
send fails. Retries reuse exact cached bytes and never repeat credential
resolution, root derivation or nonce generation. Duplicate floods cannot
advance the next emission time. Exhausting emissions stops retransmission
while allowing a delayed response until the exclusive overall deadline.

The hard setup deadline never renews. Arithmetic saturates, clock regression
closes the owner and all callbacks are followed by state/grant checks. Invalid
codec/identity/nonce/phase records are ignored; a matched attempt with an invalid
cryptographic proof, unsatisfied requirements, bad credential or changed grant
fails explicitly. There is no automatic legacy or plaintext fallback. Successful
activation retains its root/replay state. The Listener's receipt cache stops at
the original deadline or emission limit; old retries cannot reset the root.

`next_deadline` supplies the next retry, setup deadline or grant expiry. The
native owner services it with `tick` and calls `receive` only for its
already admitted unicast session. Active events revalidate the grant; a close,
expiry or policy change revokes already borrowed gates. The component has no
timer, global pending-attempt table, thread or native queue. It does not discover
silent revocation between owner calls, cancel committed coordinator work, detect
multicast failure or authorize switching. Lost final receipts may leave only
one endpoint locally active; bounded setup failure and subsequent connection
lifecycle handling must not be interpreted as delivery permission.

The bridge below rechecks local authorization, binding, source, sequence and
policy generations at coordinator enrollment/commit. Review of the provisional
wire assignment, public provisioning, detection and bounded policy reconciliation
remain prerequisites for automatic switching.

## Internal native carrier and connection owner

`attach_native_control(runtime, NativeOptions)` explicitly attaches one
`NativeService` to an established independent-source connection with a published
immutable conclusion pair. It is internal C++, behind the hybrid build gate;
building the feature does not attach it automatically. The primary handshake
and any ordinary authentication must already have completed. Attachment freezes
the actual role, established peer, socket IDs, exact conclusions and a fresh
process-local connection generation. There is no global pending-attempt table.
A published attempt cannot be replaced on the same connection, even after
failure; a new attempt requires a fresh primary connection.

The provisional local user-defined subtype is **0x7f17**. Its type-specific field
is zero and its payload is exactly one 176-byte exchange record: 192 bytes with
the ordinary 16-byte control header. Direction and destination socket are checked
against the record. This assignment has only been checked against this source
tree, not an external registry, and is not a stable interoperability contract.
Authenticated runtime controls retain their existing 0x7f16 inner packet inside
the separate version 7 ACP1 envelope, 172 bytes in total. Bare 0x7f16 packets are
consumed and discarded. Candidate envelopes are separated from primary ACP1
authentication before its replay/liveness processing. Only the established
unicast peer and unicast-session ingress can enqueue them; multicast ingress
and foreign peers cannot. An unattached/default build discards these candidates.
None refreshes ordinary SRT peer liveness or ordinary control statistics.

`NativeOptions` owns a shared callback context. Resolve and Revalidate receive
the locally derived request described above; Resolve runs once per attempt.
The context must provide a separately provisioned random 32-byte pairwise control
credential and current grant. The service never fetches DATA keys or passphrases.
It cannot establish entropy, identity authorization or member uniqueness on the
provider's behalf. Callbacks must be nonblocking and use weak references back to
the runtime/service to avoid ownership cycles. The native service serializes
protocol state and runs credential/activation callbacks without runtime, group
or queue locks. The callback context remains alive while work executes.

After local proof readiness, the `enroll` callback receives the selected context,
grant and local connection generation once. Approval is followed by another
grant check and a current runtime/conclusion binding check before the service
publishes its authenticated Session. This hook approves local control
activation; it does **not** itself implement an atomic group-coordinator enrollment or
change original/repair delivery policy. Received events are authenticated input,
not permission to commit a delivery transition. The optional bridge below adds
the current member/source/policy, sequence/history, budget and deadline checks
at its own commit boundary, after callbacks.

Each service reserves one slot on the existing bounded RuntimeScheduler. Slot
exhaustion explicitly refuses attachment; no worker or scheduler limit is added.
It owns a 16-job input queue, processes at most eight jobs per turn and exposes
an eight-event output queue. The complete owner is below 64 KiB, including its
bounded Exchange and inline Session. Input overflow drops/refuses work; event
overflow closes the service. Consumers must drain events. Scheduler notifications
coalesce, and a reserved timer services setup retries and grant expiry without
depending on the ordinary datagram retry queue. Cached setup retries retain the
existing stage limits and exact bodies, including after transient UDP failures.

Grants, arrival times, observations and local send deadlines use absolute
`steady_clock` microseconds, exposed by `NativeService::monotonic_now`. Wire
timestamps remain relative to the native connection origin. Every queued send
has an exclusive local deadline no more than five seconds ahead, additionally
bounded by setup/grant expiry. Runtime binding, cancellation and deadline are
checked immediately before UDP submission under the runtime mutex. Permanent
or partial submission closes the control service. A command encountering EAGAIN
produces a retryable event; its consumer may resubmit the identical serial/body
before a new local deadline, using a fresh crypto envelope. An expired queued
command consumes no semantic serial. There is no hidden generic FIFO retry.

STREAM_PROGRESS, PATH_REPORT and MULTICAST_READY require a local observation
time and zero initial age. The service includes local queue wait in the sealed
age and permits each report serial only once; every report attempt needs a
fresh serial. It does not automatically retry reports and thereby refresh stale
observations. Incoming events retain native arrival time as their first receive
time, including duplicates; queue delay cannot refresh it. The eventual consumer
must still assess event age, observation truth and policy freshness.

Close, breakage, peer shutdown/error or any native runtime option application
cancels the service. Close clears queued work/events immediately and never waits
for a running credential callback. That worker checks cancellation before
publishing output and erases keys on exit; an idle close erases them immediately.
Grant expiry has a reserved timer, and send/pop additionally reject expiry even
before that timer runs. Grant changes are checked on protocol work; silent
revocation between calls requires a provider-triggered close or a future policy
notification. No renewal or resurrection is supported. Control-only failure
leaves the ordinary connection running unless the trusted host explicitly closes
it. Reentrant callback close cannot publish a Session, receipt or success event.

Tests use explicit synthetic credentials, not production provisioning. They
cover every lost setup stage, forged controls, replay continuity, queue pressure,
timed observations, EAGAIN, expiry, revoked grants, binding changes and reentrant
close. Native UDP cases exercise plaintext/CTR/GCM coordinated DATA, primary
session-authenticated CTR with KM rotation, dropped selection recovery and
provenance rejection. They verify that fallback requests leave multicast policy
unchanged. These tests establish neither automatic recovery nor hostile-network,
WAN, scale or other-platform support. Existing ACP1 security-review,
shared-credential impersonation and no-forward-secrecy limitations still apply.

## Explicit sender coordinator bridge

`CoordinatorBridge` is an internal, default-OFF sender-side adapter for one
coordinated member. Building hybrid control or attaching NativeService alone
does not install it. The host uses `CoordinatorBridge::identity(actor, runtime)`
to obtain the actual source incarnation, member ID and original join position
for the Listener's selection. Enrollment checks that selection against the
current actor, original dispatch sequence, multicast destination, actual bound
source endpoint, established runtime, published conclusions and active native
owner. Only a healthy multicast source and an initially unchanged multicast
member policy can enroll. Ordinary handshake ISNs remain independent of the
dispatch DATA origin. Other profiles and source-error recovery do not enroll.

A nonblocking credential callback's returned generation alone cannot serialize
a later policy-store revocation against an actor commit. The host must supply a
shared `AuthorizationLease` containing exactly the resolved local grant. This
lease is single use; its mutex serializes revocation with enrollment and policy
commit. The host **must revoke this lease before changing or removing the
corresponding provider authorization**. Revocation cancels the native service
after releasing the lease mutex. It prevents subsequent bridge commits and
clears pending native replies; it does not undo already committed DATA work or
delivery policy. A host that also revokes DATA delivery must remove/close that
member through the existing ownership path. Opaque provider state changed
without following this lease contract cannot be atomically protected by the
bridge. Provider revalidation still runs before bridge work and native sends.

The bridge consumes authenticated events directly from NativeService. It does
not accept caller-constructed Event objects as authorization. Successful
enrollment exclusively claims the native application queue and outbound control
serials; public `send`/`pop` cannot bypass it afterwards. A service whose
application events or sends were already consumed cannot be handed over.
Attachment failure before enrollment leaves those application facilities
available; an enrolled bridge cannot be replaced on the same native attempt.

Native protocol ownership serializes credential callbacks before bridge work.
Each actor transaction holds the authorization and native-close fences, then
try-locks actor publication, multicast emission, the member runtime and the
aggregate wire budget in that order. Busy acquisition returns without mutation.
All registry/group metadata locks must already be released by the host. There
are no user credential/activation callbacks, socket sends or new allocations
inside this transaction. The emitter barrier prevents a concurrently observed
terminal source failure from authorizing a new transition. Runtime binding and
native owner identity are rechecked under the retained runtime lock. Clock,
exclusive command/grant deadlines and history TTL are checked at commit. Work
notifications and readiness updates occur after releasing these fences.

For FALLBACK_REQUEST the sender checks the exact expected policy revision and
absolute receiver position against current retained history and the source
cursor. The existing runtime guard validates the whole contiguous prefix and
key horizon. The complete prefix, including previously submitted repairs, must
fit the negotiated prefix limit; a request cannot authorize a partial replay.
The existing full-stream recipient quota and aggregate wire budget must admit
the member. A successful transaction schedules that immutable prefix, reserves
the member's unicast budget lease and changes future originals to unicast with
a new revision and current sequence/position boundary. Existing queued native
copies remain valid. Reconfirming an already granted unicast mode keeps its
policy revision. Denials return DELIVERY_STATUS with stale, history unavailable,
budget or unsupported outcomes, without changing policy or replaying a prefix.
Denial retention fields conservatively advertise no earlier recovery than the
current cursor. MULTICAST_READY is explicitly unsupported by this adapter;
reports are consumed without inferring a failure or authorizing multicast return.

The host must call `pump` outside group/runtime locks; no worker, timer or
automatic attachment is added. Each turn consumes at most eight native events,
examines at most eight pending commands and queues at most eight replies.
The fixed 64-entry request/reply cache and bookkeeping keep the bridge below
32 KiB. Duplicate serial/body requests reuse the immutable response and never
repeat the actor transition. Revalidation of source, native binding and policy
revision also precedes cached reply admission; an observed external policy
change or source failure fences the bridge. Already admitted replies and network
packets cannot be recalled, and observation between pumps can be delayed. New status
responses describe the current cursor; cached commits retain their historical
boundary and do not renew authorization or retention promises. The receiver
owner below enforces monotonic revisions and reconciles uncertainty.

Command lifetime is measured from the native Session's original receive time,
including time waiting in both queues, and is capped by the local grant expiry.
Defaults are one second and eight attempts, bounded to five seconds and eight
attempts. Deadlines never renew for duplicates. Runtime/actor contention consumes
bounded command attempts; exhaustion or expiry can discard a request without a
reply and without mutation. A fresh request serial can reconcile the current
revision. Lost replies are recovered by duplicate requests; EAGAIN produces a
bounded retry of the same response in a fresh crypto envelope. There is no
periodic scheduling or guaranteed response while the host is not pumping.
Event/queue overflow retains NativeService's fail-closed behavior. Control-only
failure does not roll back an already committed delivery policy.

Tests exercise stale/future/retired positions, policy revisions, prefix limits,
recipient quota, closed budgets, TTL, grant denial/revocation, malformed grants,
runtime contention, close, source failure/departure and exclusive queue ownership.
Native plaintext/CTR/GCM cases drop the first commit, retry the authenticated
request and verify one transition, then suppress multicast DATA while delivering
48 messages through canonical unicast originals, including CTR/GCM key rotation.
The receiver owner below adds bounded periodic policy queries. No receiver
detector, multicast return/hysteresis, periodic progress producer,
public provisioning, automatic source recovery
or startup-join fallback is connected. Loopback/synthetic-credential evidence
does not qualify hostile networks, physical/WAN paths, overload, scale or other
platforms; the existing security limits remain.

## Receiver revisions and bounded policy reconciliation

`ReceiverBridge` explicitly claims a live Caller NativeService after both
conclusion proofs and the local activation hook succeed. It requires negotiated
`policy_query` (bit 32), and exclusively owns application events and outbound
control serials. Previously used or already claimed application queues cannot
hand over; public NativeService `send`/`pop` cannot bypass this owner afterwards.
There is no public C option or automatic attachment. Requiring bit 32 in both
NativeOptions capability policies makes this dependency explicit. Older prototype
peers can reject the new capability; no silent downgrade or compatibility claim
is made. Existing profiles that select only the previous feature bits retain
their original response bodies and behavior.

The negotiated extension adds Caller DELIVERY_QUERY (kind 7). It is read-only:
the sender bridge checks current authorization/source/runtime binding and returns
DELIVERY_STATUS with the current revision, mode and cursor, without scheduling
prefix repair, changing policy or reserving a unicast recipient. Invalid or
expired history may conservatively advertise only the current cursor as retained.
A closed DATA budget does not prevent answering a query on an otherwise live
member. Queries do not prove DATA production, emission, receipt or path health.

When bit 32 is selected, sender delivery responses echo the triggering request's
serial in body word 72. That word remains `produced_next` for STREAM_PROGRESS
and zero for legacy delivery responses. The exact body stays 96 bytes and the
existing authenticated envelope stays 172 bytes. Context validation rejects
query kinds/nonzero response references without the negotiated bit. The feature
selection is bound into the existing proof/KDF context, and the complete echoed
body is covered by the existing MAC. No new cryptographic construction is added.
There is no response correlation inferred from arrival order or policy revision.

Receiver snapshots separate historical observations, a known commit boundary,
current confirmation freshness and the last explicit fallback result. Revisions
never decrease. Old revisions are ignored; conflicting modes or commit boundaries
at one revision, or a newer revision with a backwards source cursor, close the
control owner. Same-revision status cursors/retention observations never regress.
A late commit may fill a missing historical boundary, but cannot refresh a newer
confirmation or roll back its cursor. A status-only confirmation does not invent
a commit boundary. Reported cursors are sender policy/admission metadata, not
local receive/ACK cursors or original-emission high water.

Only a fresh response echoing the current request can confirm policy: the local
native owner must first report full UDP submission, the response must be newer
than previously observed reply serials and its original native receive time must
fit the current exclusive round/operation deadlines. Network/sender delay counts
conservatively from the earliest local request admission, and the original
receive time retains local queue age; semantic duplicates cannot renew it. Uncorrelated or old
cached replies can add historical knowledge without completing the current
query. This does not make an authenticated sender's claims truthful, establish
current DATA delivery or recall previously admitted network packets.

`request_fallback(next_position)` is explicit host intent. The host must supply
its validated absolute merge cursor; the owner only validates its context mapping
and never advances receive state from a peer assertion. An initial fallback
request retains its original posting deadline, including local intent queue wait,
and retries identical bytes/serial within its bounded round. If confirmation
is lost, later rounds use fresh query serials. A query showing unicast completes
mode reconciliation without rescheduling a prefix. A query showing multicast may
retry the still-explicit intent in another bounded round, preserving the original
cursor. A denial stops that intent. Its recorded result survives later background
queries. Mode confirmation does not prove that a requested recovery prefix was
delivered; the response outcome and ordinary reliability path remain relevant.

Defaults are 100 ms retry spacing, 500 ms rounds, a two-second operation,
500 ms periodic query spacing and a one-second confirmation lifetime. Each round
has at most four local send attempts and each operation at most four rounds.
Configuration bounds each to eight (at most 64 sends per operation), lifetime to
five seconds and all intervals to their validated bounds. Queue-full/EAGAIN and
duplicates cannot renew a deadline or create an immediate retry loop. Operation
exhaustion or an expired confirmation means unknown policy freshness, not a
multicast-failure decision. The next background operation is read-only; it never
restarts a denied/exhausted fallback intent. Missing control replies do not
automatically change delivery policy or fail the primary connection.

The host calls `pump` outside group/runtime locks. Each turn consumes at most
eight events and queues at most one timed request. `next_wakeup_us` exposes an
absolute monotonic wake bound without creating a timer/thread or increasing
scheduler capacity. The owner is below 4 KiB and uses constant request/revision
storage. Provider callbacks run under native protocol ownership before the
receiver mutex; callback close, runtime lifetime changes, option changes and
grant expiry retain NativeService's cancellation fences. Freshness expires in
snapshots even if the host has not pumped. Close prevents further confirmation
and leaves historical metadata distinguishable from live authorization.

Tests cover correlated/unmatched/late replies, rollback/conflict refusal, total
loss and bounded attempts, queue delay/pressure, EAGAIN without submission,
forged echoed references, duplicate freshness, capability/role/ownership refusal,
explicit-intent cursor preservation and close/revocation/expiry. Native
plaintext/CTR/GCM tests suppress every commit, reconcile by query with one actor
transition, continue periodic read-only confirmation and receive 48 unicast DATA
messages across key rotation. A separate native case suppresses all query replies,
observes unknown freshness without fallback, continues multicast DATA and then
reconfirms after replies resume. Both DATA ingress paths remain enabled under
the existing merge/duplicate rules; receiver policy metadata does not select a
path, ACK DATA, infer multicast health or implement return/hysteresis.

## Local DATA observations

The internal `CoordinatorBridge::data_progress()` and
`ReceiverBridge::data_progress()` getters expose bounded, read-only samples
behind the hybrid gate. They do not emit STREAM_PROGRESS or PATH_REPORT, infer
source activity, run a detector, request fallback or change DATA/ACK state.
A missing sample means unavailable evidence. A high water is the largest
observed absolute position plus one; it does not establish a contiguous prefix,
remote receipt, application delivery, an ACK horizon or current source liveness.
Only a strictly higher position changes `last_advance_us`; repeated and older
copies cannot renew that timestamp. Timestamps use the host monotonic clock.

The source sample separates the coordinator's admission cursor from actual
multicast and per-member unicast original UDP submissions. Admission, queued
work, EAGAIN, short sends and failed sends are not successful submissions.
Multicast advances only when the actor drains a complete emitter outcome;
unicast advances only after complete native UDP acceptance for the current
member binding. Retransmitted repairs do not count as unicast originals.
Multicast high water belongs to the source incarnation and can precede a
member's join. Unicast positions and the retained send-window floor map through
that member's join origin. This floor describes retained sequence slots; it is
not a promise that an entire recovery prefix is available. Samples can lag
completion while the actor is busy, and can be unavailable during runtime
contention, detachment or failure. A false `source_failed` flag does not prove
health or production.

The receiver separates total unique DATA from validated unicast and multicast
observations, including matching multicast copies after unicast application
consumption. Existing source/peer and route-generation checks, crypto processing
and core receive admission precede observation. Encrypted DATA must have usable
plaintext; optional-encryption discards and filter-supplied/control packets do
not count. Plaintext and CTR retain their existing integrity limitations.
The receive window supplies only the lifting anchor across 31-bit wraps;
window skips, application reads and ACK progress do not manufacture DATA
observations. Ambiguous/backward anchors or absolute-position overflow make the
sample unavailable.

A fixed cache holds exact normalized DATA headers and accepted plaintext for
32 recent positions. The retransmission flag is normalized; changed payloads,
other header changes, uncached old copies and repeated copies on the same path
cannot advance path progress. The cache allows comparison after the application
has consumed the packet without inventing a hash or authentication mechanism.
Receiver unicast observations include valid repairs, unlike source unicast
original-submission marks. One sparse high position can evict older evidence;
this bounded cache does not prove continuous path coverage. Plaintext may remain
in this cache until replacement or runtime destruction.

The observer is smaller than 64 KiB and is allocated once when an HC-enabled
independent Caller publishes its immutable conclusions; allocation failure
fails setup explicitly. Default-OFF builds add no observer allocation or runtime
field. There is no new thread, timer or queue. Getter access requires the live
native root and current route epoch; revoking or replacing that route hides its
observations rather than carrying them into a new epoch. Closing the receiver
owner or revoking the sender authorization lease hides its sample. These are
local snapshots, not an authorization transaction or a fresh provider check;
the existing revoke-before-provider-change contract remains mandatory.

Next work is bounded authenticated reporting with an explicit source-activity
contract, followed by detection and hysteresis based on evidence age. Policy
query cursors remain admission metadata and cannot substitute for DATA evidence.

## Control body

Every control body is exactly 96 bytes.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Version, 1 |
| 4 | 4 | Kind, 1 through 7 |
| 8 | 8 | Stream incarnation |
| 16 | 8 | Member incarnation |
| 24 | 8 | Nonzero semantic serial |
| 32 | 8 | Policy revision |
| 40 | 8 | Next absolute sequence position |
| 48 | 4 | Matching 31-bit next sequence |
| 52 | 4 | Mode: multicast 0, unicast 1, overlap 2 |
| 56 | 8 | Retained floor |
| 64 | 8 | Multicast next position |
| 72 | 8 | STREAM_PROGRESS produced next; negotiated delivery-response request serial; otherwise zero |
| 80 | 4 | Activity: startup, producing, paused, ended, source failed (0–4) |
| 84 | 4 | Outcome: OK, denied, history unavailable, budget, stale, busy, unsupported (0–6) |
| 88 | 4 | Observation/report interval in microseconds |
| 92 | 4 | Report age in microseconds |

Sender kinds are STREAM_PROGRESS (1), DELIVERY_COMMIT (4), DELIVERY_STATUS (5).
Receiver kinds are PATH_REPORT (2), FALLBACK_REQUEST (3), MULTICAST_READY (6)
and negotiated DELIVERY_QUERY (7).
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
gate alone has no coordinator adapter, retry scheduler, failure detector or public
socket option. The explicit sender bridge above validates policy revision,
history, deadlines and budgets before its bounded coordinator transaction. Peer
claims can still be false; authenticated traffic alone is not source DATA,
source liveness or multicast-failure evidence.
