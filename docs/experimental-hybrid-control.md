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
revisions and reconciles policy with bounded read-only queries. Explicitly configured
owners can also publish authenticated progress/path reports from local DATA
observations and bounded host activity declarations. An explicitly configured
passive receiver detector assesses stalls and sustained multicast recovery.
A separately attached internal controller can opt into bounded automatic unicast
fallback using trusted local merge state. A separate opt-in return controller uses
sustained recovery and exact committed-overlap coverage. An internal staged-startup
membership owner can retry a missing join while authenticated unicast remains usable.
Default configuration keeps automation
disabled and allocates no public C option. Native handshakes still
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
./build-hybrid-control/robotweax_srt_tests --include hybrid_reporting
./build-hybrid-control/robotweax_srt_tests --include hybrid_detector
./build-hybrid-control/robotweax_srt_tests --include hybrid_automatic
./build-hybrid-control/robotweax_srt_tests --include hybrid_return
./build-hybrid-control/robotweax_srt_tests --include hybrid_auto_return
./build-hybrid-control/robotweax_srt_tests --include hybrid_membership
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
responses. Bit 64 selects bounded return control and effective-boundary metadata;
it requires bits 8 and 32. The previous all-features mask remains 63 and the
return-only mask is 127. Bit 128 permits the internal staged-startup membership
profile and requires bit 32. The current known mask is 255. Unknown bits are
rejected, required bits must be supported, and a
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
dispatch DATA origin. Other profiles and already failed/recovered sources do not
enroll new control owners; existing UC owners can reconcile explicit recovery
as described below.

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
The receiver, reporting, detection, fallback, return and staged membership owners
below attach separately. The explicit startup provisioning profile later in this
document integrates group connect admission. Source-error control integration is
described below; production credential discovery remains unimplemented.
Loopback/synthetic-credential evidence
does not qualify hostile networks, physical/WAN paths, overload, scale or other
platforms; the existing security limits remain.

## Native source-error control integration

An already enrolled `CoordinatorBridge` retains authenticated control after a
permanent native multicast transport error or partial datagram. Internal actor
faults, unrelated detachments, retired runtimes, changed roots, revoked grants,
expired credentials and external policy changes retain their fail-closed gates.
This does not enroll a new owner on a failed or recovered source. Reporting still
requires an explicit nonzero reporting interval and host pumping (or the existing
provisioned startup service); no new timer, thread, queue or public selector is
added.

The coordinator supplies `Activity::source_failed` from the actual permanent
native error, including after explicit recovery leaves the failed multicast
source retired. This describes the multicast source, not failure of the surviving
UC session. It does not require or infer a producing declaration. The produced
cursor is at least actual application admissions and preserves any larger earlier
explicit production declaration, even an expired one; it never manufactures a
successful UDP submission. Ordinary produced/submitted/multicast marks, member
policy and retained floor remain distinct. A fresh sample of the still-current
permanent failure may produce a fresh bounded diagnostic, while DATA advancement
timestamps change only on actual new submissions. Ended may advance to failure;
failure cannot revert to producing, paused, ended or startup in this incarnation.

The failed profile answers fresh correlated `delivery_query` requests with the
actual current policy. Fallback and return commands receive `denied`, including
for a member detached only as `source_not_submitted`; they neither approve UC nor
restart the source. The receiver detector exposes `source_failed` and suppresses
both fallback and return candidates. An MC member can therefore retain diagnostic
control while its original DATA path remains broken. A control report or query is
never recovery authorization or proof of delivery.

`recover_source_to_unicast` remains an explicit trusted host operation with its
existing atomic selected-member, source/member revision, full retained-prefix,
key, outcome-drain, quota and bandwidth checks. No automatic recovery selection
or new authenticated recovery command is introduced. For a selected member that
already had a live, unchanged UC delivery lease under this control root, the
actor records a fixed receipt for the old source/member revisions. The same
bridge alone can adopt the exact single successor source and member revisions,
confirm UC through a fresh query, and continue reporting. A selected MC/overlap
member, an unselected member, a missing/wrong receipt, an extra revision change or
an unrelated delivery change cannot be adopted. Existing local recovery may still
restore such a primary session, but its former bridge closes rather than treating
that restoration as authenticated UC admission.

The source recovery receipt does not relax grant expiry or revocation and does
not count as a new bridge-applied remote transition. Pending cached replies from
older revisions are suppressed on adoption and cannot be rearmed by duplicate or
retry events. Already queued or submitted datagrams remain historical and cannot
be recalled; receiver correlation and monotonic revision checks remain mandatory.
After recovery, control is read-only for delivery policy: no multicast return or
new prefix command is accepted. Closing/revoking control leaves committed UC DATA
under the ordinary primary runtime and actor ownership.

Components cover failed/partial sends, diagnostic-only queries, denied mutations,
exact recovery receipts, cached duplicates, internal faults, revision changes,
revocation, sticky failure reports and detector suppression. Native loopback tests
exercise plain/CTR/GCM, lost UC commits recovered by query, permanent MC error,
authenticated reports before/after explicit recovery, 80 canonical UC messages
across key rotation, control revocation with further DATA, and a provisioned
startup owner driven by its existing service. No production credential discovery,
unprovisioned membership-loss recovery, hostile-network, WAN, scale or playback
continuity qualification is implied. The managed post-join recovery profile below
requires separate explicit configuration.

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
contention, unrelated detachment or internal failure. Native transport failure
may retain a diagnostic sample for an enrolled member detached solely because
its multicast original was not submitted. Such a sample has no invented successful
submission marks. A false `source_failed` flag does not prove
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

Policy query cursors remain admission metadata and cannot substitute for DATA
evidence. The optional report producers below consume these local samples.

## Explicit authenticated progress reporting

Both bridge configurations have a `reporting` policy. Its interval defaults to
zero (disabled). A nonzero interval must be at most one second, no greater than
the report lifetime; lifetime must be positive and at most five seconds. The
500 ms lifetime field is a configuration bound, not a production detection
threshold. The host must choose timing against its latency and scheduling
budget, pump both owners and honor `next_wakeup_us`. Sender wakeup currently
exposes report production timing; existing pending command replies still need
regular pumping. No timer, thread or scheduler service is added.

Before STREAM_PROGRESS can be produced, the trusted source host must call
`GroupSendCoordinator::declare_source_activity(activity, produced_next,
lifetime_us)`. This describes actual application production in absolute source
positions, which can lead transport admission. It is an explicit local assertion,
not a value inferred from queued sends, source silence, read-only queries or a
remote peer. The declaration is shared by members of that source incarnation.
It must cover current admissions and cannot move the produced cursor backwards.
Startup requires zero production and cannot resume after another activity;
producing and paused can alternate. Ended fixes the final production cursor and
is terminal for that incarnation, even after declaration expiry. The host cannot
assert `source_failed` through this method. Actual permanent native transport
failure supplies that diagnostic through the source-error integration above. Invalid declarations do not replace the last valid declaration.

Declaration lifetime is positive and at most five seconds. The source clock
captures declaration time internally. Refresh declarations while they remain
true. A report's observation origin is the earlier of declaration and DATA
sample times, and its deadline is bounded by both declaration expiry and the
report lifetime from that origin. Repeated pumps cannot turn an old assertion
into fresh activity. A later admission beyond the declared produced cursor makes
activity unavailable; it does not block or change DATA admission. Such admissions
after an ended declaration cannot be repaired by inventing a larger final cursor.
The source and native service use the same host monotonic clock domain.

STREAM_PROGRESS carries the declared production cursor separately from the
largest actual original UDP-submission position across multicast and this
member's unicast originals, and separately from multicast submissions. No
observed submissions map to the member's initial position; pre-join source
multicast marks are clamped to that origin. The retained floor is the local
send-window floor, not a recovery guarantee. Member mode/revision and DATA marks
come from one coordinator snapshot. A valid declaration plus queued DATA can
therefore report producing with no advancing submission mark. Paused/ended
production can coexist with later submission of previously produced backlog.
No activity assertion proves remote receipt or peer honesty.

PATH_REPORT uses validated unique DATA and per-ingress multicast high waters,
including exact matched multicast copies after unicast consumption. It never
promotes receive-window, drop or ACK cursors to DATA. If the live runtime or
original route epoch cannot supply an observation, the receiver skips reporting.
It may report the last confirmed policy revision/mode as historical metadata;
this does not select paths or confirm sender policy. Reports already admitted
before an epoch/state change remain historical samples with their original
observation time and exclusive deadline; they cannot be recalled from the network.

Commands/replies take priority over one timed report attempt per pump. At most
one report per owner remains queued/in flight. Each due attempt, including full
queues and unavailable samples, advances the next attempt time; pumping cannot
create an unbounded report queue. Source reports use the sender reply serial
space; path reports use the receiver request serial space. They share the existing
64-entry semantic windows, so very frequent reports can age old command retries
out; a fresh query can reconcile. Each admitted report attempt has a fresh serial.
Queue expiry, EAGAIN and rejection end that attempt; no report body is retried
with a changed age or timestamp. Native sealing adds sender queue age before the
existing v7 authenticated envelope is submitted. Existing provider/root/expiry
and authorization lease fences apply. Report production does not reserve DATA
bandwidth or commit a prefix.

`peer_report(maximum_age_us)` exposes one authenticated peer report and its
freshness. It requires a live owner; freshness requires a positive age limit at most five seconds.
Freshness uses advertised age plus elapsed local time since the original native
receive, including event-queue wait, with an exclusive age bound. Network transit
before that receive is not measured; this is not a synchronized clock protocol.
Missing reports return no observation; stale stored reports return `fresh=false`.
Duplicates, older serials, backwards positions/revisions and conflicting
same-revision modes cannot replace or refresh the stored observation. Clock
regression makes its tracker permanently stale. Ended reports cannot reopen
production or change the final produced cursor, but can advance to actual source
failure. Source-failed reports cannot return to another activity in the same
incarnation; later UC submissions can still advance DATA marks. Peer claims
remain assertions.

Metadata freshness and DATA advancement are distinct. A new paused report can
renew metadata freshness while its `next_advanced_at_us` and
`multicast_advanced_at_us` remain unchanged. Those timestamps are local first
receive times for strictly increasing reported high waters, not remote DATA
arrival timestamps or proof of a contiguous prefix. Incoming reports neither
confirm a delivery query nor create fallback intent or mutate DATA/ACK state.
Closing/revoking an owner hides its peer report. The optional passive detector
below consumes these observations; explicit one-way fallback automation is
provided separately by the controller described below.

## Passive stall detection and recovery hysteresis

`ReceiverBridge::Configuration::detection` defaults to absent. Explicitly
configuring it allocates one fixed-size detector (below 2 KiB); allocation or
configuration failure refuses receiver-owner creation without changing DATA.
There is no public C option, new wire field, thread, timer or scheduler service.
`path_assessment()` returns no value when disabled or the native owner is
closed/revoked/expired. An unavailable original DATA route yields an unknown
assessment. The host still pumps the receiver owner; `next_wakeup_us` includes
its sampling bound. No detector code posts a fallback request, MULTICAST_READY,
policy command, merge cursor or ACK, or disables either DATA ingress.

All nine configuration fields are required explicitly. Times are positive and
at most five seconds; `recovery_advances` is between 2 and 64. There are no
production defaults or automatic thresholds derived from the negotiated latency.
The host must choose and validate timing against its application latency,
control scheduling, network behavior and possible recovery cost:

| Field | Meaning |
| --- | --- |
| `report_age_us` | Exclusive report-age bound including advertised age, elapsed local receive time and the configured transit allowance |
| `maximum_transit_us` | Assumed maximum one-way network transit for control/DATA; this component does not measure or enforce it |
| `stall_us` | Silence budget after observing a missing submitted horizon; a candidate needs this plus the transit allowance |
| `emission_lifetime_us` | How long an observed increase in actual submission position can support stall/recovery assessment |
| `recovery_us` | Required span between first and last qualifying multicast DATA advances |
| `recovery_gap_us` | Exclusive maximum age/gap for recovery progress |
| `hold_down_us` | Minimum hold after a stall or establishing a unicast policy baseline |
| `sample_gap_us` | Exclusive maximum interval for retaining assessment evidence without another owner sample |
| `recovery_advances` | Minimum distinct multicast DATA advances and distinct advancing multicast submission reports in a recovery window |

Validation requires transit and sampling bounds no larger than `stall_us`,
sampling no larger than recovery gap, and recovery gap no larger than recovery
span. Report age must exceed transit plus sampling. Emission lifetime must
exceed stall plus transit plus sampling. The complete detect/control/replay
budget can still exceed playback slack; these checks do not promise continuity
or qualify a production network. Incorrect assumed transit bounds can produce
incorrect assessments. Reports do not provide synchronized clocks or independent
proof of peer honesty.

Each assessment requires an authenticated fresh STREAM_PROGRESS, an available
local validated DATA snapshot, and fresh correlated policy confirmation whose
revision/mode matches that report. Missing/stale reports, unknown policy,
inconsistent identity/positions, changed route or sampling delay produce unknown
or a new observing baseline. Clock regression or replacement native generation
permanently invalidates that detector. Fresh evidence after an interruption must
establish a baseline and then advance again. Reports with altered content/first
receive time under the same serial, backwards horizons or changed timestamps for
unchanged local high waters cannot refresh evidence.

The first positive report establishes a baseline. Only a later strict increase
in the actual multicast submission horizon arms multicast emission evidence.
Produced/admitted positions, retained/receive-window floors and repeated reports
with stationary submission horizons cannot arm it. Multicast is suspect while
a freshly advancing submitted horizon is ahead of local multicast observations.
The silence timer starts when that gap is first observed and restarts only for
an actual higher local multicast observation. After `stall_us` plus transit it
marks `multicast_stalled`. A separate aggregate timer compares all original
submissions against all validated DATA; `all_data_stalled` means no advancing
aggregate DATA against that horizon, not proof that two physical paths failed.
A confirmed multicast policy with multicast stall can expose
`fallback_candidate`; it never creates fallback intent.

Submission evidence ages from its original local report receive time. Fresh
stationary metadata cannot renew it. When that emission evidence expires, old
blackouts cannot remain candidates. Source activity claims yield distinct
startup, paused, ended and source-failed states and suppress candidates. This
is conservative during paused/ended production even if a backlog remains;
ordinary reliability handling is unchanged. A source-failed report remains a
peer assertion; the source-error bridge above now produces it from actual native
transport failure without authorizing a receiver transition.
No fresh emission increase gives `awaiting_emission`, which is not a path outage.

After a stall, or while the confirmed policy is unicast, recovery requires
strict increases in local multicast DATA and authenticated multicast submission
reports. Each side must reach `recovery_advances`; repeated copies or stationary
metadata do not count. Local advances must span `recovery_us`, stay within the
recovery gap and sampling/age bounds, and outlast hold-down. The local multicast
high water must also catch the reported multicast horizon. A confirmed unicast
policy can then expose `multicast_ready` and `return_candidate`, while sender
policy stays unicast. Insufficient progress, another stall, stale evidence,
pause, policy revision change or excessive gaps clear the recovery window.
A ready assessment expires without continued observations or pumping.

Assessments include observation time, exclusive validity deadline, next wakeup,
policy revision, separate source/local high waters and recovery counts. Getter
reads project expired assessments to unknown and clear both candidate flags;
those reads never count as progress or send commands. `multicast_progress` and
ready states describe bounded high-water evidence, not continuous coverage,
loss-free delivery, a retained replay prefix, key availability, available
bandwidth or an overlap commit boundary. Native loopback qualification exercises
blackout detection, explicit host fallback, sustained multicast observation
while unicast remains committed, pause suppression and owner closure. The
separate controller below can consume fallback candidates. Explicit bounded
multicast overlap/return and its separate opt-in automatic controller are described
below. The internal staged-startup membership profile is described later; public
credential discovery remains unimplemented. The opt-in group setup profile below
now integrates staged startup admission.

## Explicitly attached automatic unicast fallback

`AutomaticFallback::create(receiver, configuration, provider)` is an internal
opt-in controller behind the existing default-OFF HC/ACP1 gates. It requires an
attached live Caller receiver with a configured detector. Creation is exclusive
and single-use for that receiver lifetime: closing/destroying the controller
cannot reset its allowance by attaching another one. Default sockets, native
owners and detectors do not create controllers. There is no public C selector,
new handshake capability, wire record, key, timer, thread or scheduler service.
The controller has fixed storage below 2 KiB and holds its existing receiver;
provider storage/allocation behavior remains host-owned.

The host explicitly configures `maximum_operations` in 1..8 and a positive
`cursor_lifetime_us` no greater than five seconds. One operation allowance is
spent before each cursor callback, including a missing/invalid cursor or failed
final admission. At most one such preparation is allowed for a confirmed
multicast policy revision. Candidate flapping, stale evidence, denial, exhaustion
or provider unavailability cannot rearm the same revision. A later confirmed
multicast revision can use another allowance, up to the lifetime maximum. There
is no new autonomous retry policy; an accepted operation uses the receiver's
existing bounded retry/query rounds and sender quotas/history/key checks.

`pump()` advances the receiver first and processes at most one automatic
preparation/admission. It returns immediately for concurrent/reentrant pumps.
The host must keep pumping the controller/receiver and sender bridge and use the
receiver's `next_wakeup_us`. No background automation thread is created.

A candidate must still have fresh correlated multicast policy confirmation,
a matching native generation and source/member identity, a live original DATA
route, and current detector evidence. Capturing it grants no DATA/ACK authority.
The provider receives the root identity, policy revision, initial absolute
position, local start time and exclusive admission deadline. It must resolve a
**trusted locally validated application merge cursor**, with observation time
and matching native generation. The request deliberately supplies no remote
horizon to use as that cursor. Neither a peer query cursor nor received/emitted
high water is proof of a valid merge position.

The provider callback must be nonblocking and uses the host monotonic clock. It
runs outside controller, receiver, native protocol, runtime, group and queue
locks. It may inspect state, post a manual request, close an owner or reenter
`pump()`; no locks are held around it, and reentrant pumping does no work. Use
weak back references to avoid cycles. Closing the controller does not wait for a
running callback; a late return cannot admit work after that close.

After the callback the receiver revalidates the root, candidate, confirmation,
policy revision, original admission deadline, route and cursor. Detector
resampling is pure and invokes no provider or native protocol work. New DATA,
source activity or policy can clear the candidate. The cursor must not be future
or stale, must be at least the selected initial position and no greater than the
current core receive-window floor. That floor is only a ceiling, not cursor
authority; the host must validate skipped/dropped/application merge semantics.
The controller never clamps or manufactures a cursor. Sender history/budget
refusal remains an explicit denial rather than silently advancing past data.

A successfully posted intent transfers ownership to the receiver, with a unique
ticket and an immutable local deadline. Its origin is the earlier of preparation
and cursor observation; its deadline is bounded by the receiver operation
lifetime from that origin, cursor lifetime and native grant expiry. Provider
latency, old cursor age and local posting delay consume that bound. Queues or
retries cannot renew it. A manual request accepted during cursor lookup wins,
even if that manual operation finishes before the callback returns. An active
fallback intent is never replaced by automation. Host manual requests otherwise
retain their existing API and bounds.

Controller snapshots expose operations/provider calls, admitted count, revision,
ticket, selected cursor, local deadline, outcome and closed/exhausted state. A
separate receiver receipt tracks that ticket: subsequent manual fallback or
background query results cannot overwrite it. Local deadline or root expiry
projects a pending automatic result to unknown even before another pump.
Unknown does not mean the sender refused or rolled back a command.

**Closing this controller stops new automatic preparations/admissions. It does
not cancel a transaction already handed to the receiver.** That transaction may
still commit and continues only within its original local retry bound while the
receiver is pumped. Closing the receiver/native owner stops its future control
work using the existing lifecycle fences, while primary DATA and already
committed policy remain intact. Already selected/submitted/network datagrams
cannot be recalled; a delayed authenticated request can reach the sender after
the local deadline. No absolute remote commit cutoff or distributed cancellation
is added by this feature.

Loopback qualification covers automatic blackout fallback with every commit
response suppressed: bounded read-only query reconciliation confirms unicast,
exactly one actor transition occurs, and canonical DATA continues over unicast
across CTR/GCM rotation. Component cases cover invalid/scoped/stale cursors,
callback delay/reentrancy/close/route loss/pause, manual intent precedence,
per-revision/lifetime budgets, persistent ticket results, posting-age expiry,
EAGAIN and total reply loss. The detector's assumed timing/transit bounds and
peer-honesty limits still apply. This fallback controller ignores return candidates.
The explicit return protocol and separately attached return controller below
supply overlap and coverage validation; attachment of either controller does
not attach the other.

## Bounded overlap and explicit multicast return

Return control requires negotiated feature bit 64 (`return_control`) together
with overlap bit 8 and policy-query bit 32. It reuses MULTICAST_READY with explicit
new semantics only when selected. Capability/context/body/envelope sizes remain
48/192/96/172 bytes and the existing proof/MAC construction covers the selected
bit and complete records. Older strict prototype peers may reject the bit;
required-feature negotiation fails explicitly. Legacy31 and policy-query63
fixtures retain their formats and known answers. Native primary handshakes
continue rejecting authentication v7; no public selector or crypto primitive is
added.

The sender bridge must also receive an explicit `ReturnPolicy`. All fields are
positive: overlap duration at most five seconds, overlap packets at most 256,
minimum coverage packets no greater than that cap, minimum coverage interval at
most one second and less than overlap duration, and maximum proof age no greater
than overlap duration. No policy is selected by default. Existing recipient and
aggregate UC wire quotas still apply; entering overlap retains the UC reservation
and does not schedule a prefix or add another recipient.

The host calls `ReceiverBridge::request_overlap()` only with fresh confirmed UC
policy. With bit 64 selected it sends a timed ready intent for overlap using the
current revision and empty interval at the selected initial position. The sender
commits overlap at its actual next admission position **B**, increments revision,
and records a native-root-bound overlap window. Future admissions continue
canonical UC originals while the source continues multicast. Already admitted
copies and their deadlines remain unchanged.

The window expires by source monotonic time or before an admission beyond its
packet cap. Expiry restores UC, increments revision and preserves its reservation.
It does not switch to multicast. Source admission, existing actor work and sender
bridge transactions enforce expiry; the existing actor scheduler service polls
while a window remains active, and bridge wakeup exposes its deadline. No new
service/thread/timer is reserved. Exactly the allowed number of overlap packets
may be confirmed at the cap before a further admission; the next admission
restores UC if return has not committed. Revision headroom is required so expiry
cannot wrap. Retries under the old revision cannot reopen or extend the window.

With bit 64, delivery responses carry the policy's effective absolute boundary
in word 64. Word 40 remains the admission cursor and word 72 the request echo.
A commit has boundary equal to its commit cursor; queries can report a later
admission cursor with the original boundary. The receiver treats boundary/mode
conflicts under one revision as a control error. Thus losing a commit reply can
be reconciled by a correlated query without inventing B from current admission
progress. Without bit 64 response word 64 remains zero.

After overlap is confirmed, the host supplies a `CoverageProof` to
`request_multicast()`. It asserts **contiguous validated multicast coverage of
[B,C)** under that exact overlap revision and native generation, with positive
observation interval, original observation time and exclusive local deadline.
C must exceed B and be no greater than current locally observed multicast high
water; that ceiling is not proof of contiguity. The sender requires its own
window/root/revision/B, sufficient coverage count/interval, fresh proof, and C no
greater than admitted source position. It checks expiry again at commit and
clips command/reply lifetime by original advertised proof age plus local queue
time. This is a trusted peer assertion; authentication does not independently
establish honest coverage.

`coverage_proof(C, lifetime_us)` can construct that assertion from the existing
fixed 32-position exact-copy cache. Every position B..C-1 must have an admitted
MC path copy, including exact matching MC copies after UC consumption. A hole,
changed/uncached copy, wrong route epoch or interval outside the recent cache
returns no proof. Each cache entry retains its first MC receive time; same-path
replays cannot renew it. The proof uses the minimum/maximum actual MC receipt
times for that interval, with observation origin at the last such receipt and
deadline bounded by requested lifetime and grant expiry. Zero-span intervals
return no proof. This adds only fixed entry timestamps, keeping the observer
below 64 KiB. It retains plaintext/header cache semantics and existing integrity
limits. Larger intervals require a separate trusted host ledger; high waters
alone cannot substitute for it. No receive/ACK/drop cursor changes are made.

A valid final request commits multicast at the sender's current next admission
position **D**, with another revision. Only future originals stop using UC.
Pending UC originals retain their canonical packets, pacing and completion
ownership; their reservation remains until they drain. No outstanding copy is
cancelled or rewritten, no prefix is replayed by either return phase, and
ordinary UC repairs remain available. Coverage B..C does not promise future
path reliability or establish delivery/playback deadlines for all packets.
The host must still validate application merge/drop semantics and recovery cost.

Ready attempts are timed messages. Every admitted retry uses a fresh semantic
serial and authenticated envelope while keeping the original proof observation
and operation deadline; they never retry a changed age under one serial. Sender
revision checks make fresh-serial retries idempotent. Read-only query rounds
reconcile lost overlap/final commits and their exact boundary. Unknown/denied
return results are separate from fallback results. Missing replies, expired
proof/window, stale revision, insufficient history/keys/budget or loss of owner
cannot authorize release of UC. Already submitted datagrams cannot be recalled;
local deadlines still do not imply an absolute distributed cancellation cutoff.

Native loopback tests drop every commit reply through UC -> overlap -> MC across
plain/CTR/GCM, use the exact cache to prove eight overlap positions, and deliver
40 more canonical messages after return across key rotation. Sender UC-original
high water stays at or below D. Explicit return APIs perform no autonomous
recovery selection; `AutomaticFallback` remains one-way. The separate controller
below supplies opt-in return selection. The internal membership profile below
supplies staged startup support, and the setup profile below integrates native
group connect admission. Source-error control integration is described above.
Production credential discovery and broader network/scale qualification remain
future work.

## Explicitly attached automatic multicast return

`AutomaticReturn::create(receiver, configuration)` is an internal opt-in for a
live receiver with detection and negotiated return control. All configuration
fields default to zero and fail validation. The host supplies a lifetime allowance
of 1..8 operations, a positive total operation timeout <=5 seconds, coverage of
2..32 packets, a positive minimum coverage span <=1 second and smaller than the
operation timeout, and a positive proof lifetime no greater than that timeout.
The sender still needs its independent `ReturnPolicy`; receiver settings do not
negotiate or relax the sender's bounds. A sender requiring more than 32 coverage
packets cannot be satisfied by this cache-based controller.

Attachment is exclusive and single-use for that receiver lifetime. Closing or
recreating a controller cannot reset its allowance. It may coexist with the
separately attached `AutomaticFallback`; both use the receiver's existing shared
bounded operation owner, so their intents and manual intents cannot overlap.
Each pump advances the receiver first and admits at most one phase. Concurrent
or reentrant pumps return. One controller below 2 KiB, no callbacks/providers,
new timer, thread, scheduler service, DATA queue, wire field or public C option.

Only a fresh confirmed UC revision with the detector's sustained return
candidate can consume an allowance. Its native generation, stream/member,
revision, initial position and manual epoch are captured. Admission resamples
recovery under receiver ownership. Even failed admission consumes the attempt;
the same UC revision cannot rearm on denial, timeout, flapping or repeated pumps.
A higher confirmed UC revision may consume another remaining allowance.

The first phase posts overlap through the existing bounded ready/query exchange.
No MC request is made until a correlated receipt confirms the exact overlap
revision and boundary B. Missing commit responses are recovered by read-only
queries. The second phase walks the exact cache for every position in
[B,B+coverage_packets). A high water, UC-only position, missing entry, stale
route, cache eviction or same-path replay cannot supply this coverage. A valid
matching late MC copy after UC can fill a hole; its first MC receipt sets its age.
The span must meet the receiver's minimum and original last receipt must remain
within the configured proof lifetime. Fresh producing activity at the overlap
revision, no current stall, and local MC progress caught up to reported source
MC progress are also required. These high-water checks constrain current
recovery evidence; only the exact cache supplies the contiguous proof.

Both admissions recheck native lifetime, current revision/confirmation, shared
operation availability and the original manual epoch. An accepted manual intent
wins even if it completes before another controller pump. The entire operation
retains its original start/deadline, clipped by receiver operation timeout and
grant expiry; coverage collection, lost-reply queries and posting delay consume
that bound. Final proof age may shorten it further. Sender window, authorization,
history, key and budget fences remain authoritative. Expiry or denial never
manufactures MC confirmation or starts an unbounded retry. Source window expiry
independently restores UC while preserving queued UC work.

Snapshots expose whole-operation and separate phase results, original UC
revision, overlap B, proven coverage end C, actual final commit D, admitted phase
counts and deadline. Ticket-specific return receipts persist independently of
manual outcomes and automatic fallback receipts. Reads project expiry to unknown
without pumping or posting. Final D can exceed C; future UC release follows D,
not the proof horizon. **Close stops new admissions but cannot cancel an already
posted phase.** An accepted final MC phase may still commit; an accepted overlap
phase may still open its bounded source window, but a closed controller never
posts the final phase. Primary DATA and committed policy remain intact.

Deterministic cases cover configuration/attachment, exact holes and late copies,
original proof age, paused activity, manual precedence, denial attribution,
per-revision/lifetime budgets, route/native loss, close, posting delay and grant
expiry. Native plain/CTR/GCM tests suppress every commit response, recover UC,
observe sustained MC while in UC, automatically open overlap, prove eight exact
positions, confirm MC by query, and deliver forty more canonical messages across
key rotation. This remains loopback qualification under explicit host timing,
truthful activity and pumping contracts, not WAN, startup join, hostile-peer,
scale or playback-continuity qualification.

## Internal staged-startup membership recovery

`MembershipRecovery::create(service, configuration, provider)` is a separately
attached internal owner for initial missing membership. Negotiated bit 128
(`startup_fallback`) requires policy queries; it does not require return control
or introduce another message kind. The capability/context/body/envelope sizes
remain 48/192/96/172 bytes, and existing transcript authentication covers the
selected bit. Legacy masks 31/63/127 and crypto vectors retain their semantics.
Optional startup support can downgrade; required startup support fails admission
when absent. All experimental gates remain default-OFF.

**Unprovisioned public connect still fails a local multicast join failure.** This
owner operates on an explicitly staged runtime with frozen conclusions, an active
Caller hybrid service and an established-peer UC dispatch route. The provisioning
profile below now stages that runtime inside native group connection setup and
awaits sender-approved UC before publication. Its initial policy is explicitly
UC first, with membership attempts after publication. The earlier membership-only
tests stage absence after ordinary connect; the newer setup tests exercise the
actual public connect boundary.

Attachment requires no active MC route, no previously accepted DATA, an unchanged
initial core receive floor, matching native generation/conclusions/runtime, and
a verified UC dispatch route. It is exclusive and single-use for that runtime
lifetime. A receiver cannot discard prior observations or recreate the owner to
obtain a fresh startup ledger. The staged setup owner must retain its UC route
for the primary session lifetime. The membership owner does not own that route.

The explicit local configuration supplies an interface, maximum attempts 1..8,
a positive total retry timeout <=5 seconds, and positive initial/capped backoff
with initial<=cap<=1 second and initial<timeout. Defaults fail validation. First
attempt is immediate; failed preparation schedules a bounded exponential backoff
from completion, clipped by the original deadline/grant. Every attempt consumes
an allowance, including unavailable/late preparation. At most one preparation is
made per pump; concurrent/reentrant pumps return. Snapshots expose attempts,
failures, phase, joined state, deadline and next wakeup, including grant expiry.
No retry timer or retry thread is created: the host pumps the owner. One fixed
owner below 2 KiB holds at most one published receive channel; successful native
publication uses the ordinary bounded channel/watch/worker machinery.

The default provider opens, reuses, binds and joins a new native UDP socket on the
explicit local interface. An optional trusted local provider can prepare an
actually joined, bound, unconnected socket; it must be nonblocking and must not
publish/start a route. It runs outside owner, protocol, runtime, channel and group
locks, and can inspect/reenter/close the owner. Use weak back references to avoid
cycles. Native membership success is the provider's local assertion, not evidence
of source DATA, contiguous coverage or future reliability. Socket family/port,
root, original deadline, grant and owner liveness are checked again before
publication. Late results are discarded; provider latency cannot refresh time.
A partial/expired publication is terminal and never resets the DATA ledger.

Before a successful join, an opaque runtime-bound ownership identity permits
validated UC observations while no MC binding exists. It does not admit MC:
missing/stale/foreign route identities, wrong sources and wrong UC peers retain
all existing checks. An unrelated route cannot replace a managed route. A
successful owned registration advances the route epoch while preserving the
existing UC cache and core receive state. First matching late MC copies can then
be observed normally; exact coverage still requires an actual current MC binding.
Unrelated route revocation after publication permanently hides that owner's
observations. Initial-join configuration defaults to no post-join recovery;
the explicit managed-loss profile below can retain the same owner and ledger
across bounded recovery episodes. Primary-session reconnection remains separate.

The existing `AutomaticFallback` can now assess a startup blackout from empty
local MC history under this explicit profile: fresh source production/emission,
stall timing and a trusted local initial merge cursor are still required. The
sender independently approves UC under authorization/history/key/resource and
budget fences. A source horizon never supplies the cursor; a cursor beyond the
actual core floor is refused. Join failure alone grants no UC and changes no
sender policy. Exhausted/expired retries can leave approved UC usable. After a
successful join, sustained validated MC and the separate `AutomaticReturn` can
perform the bounded overlap/coverage protocol. Neither OS join success nor a
local membership state directly requests return to MC.

Close revokes only this owner's MC route/observations and retires its channel
without waiting for a provider. It does not close the primary UC session,
cancel a handed-off fallback, request fallback, or change sender policy. Arrange
approved UC before closing a joined membership if continuous reception is
required; closing a local MC route while MC remains committed removes that local
reception path. Already admitted/network DATA cannot be recalled. Root/grant
expiry, clock regression and external route loss fail closed; a replacement
session/explicit future lifecycle policy is needed to rearm.

Deterministic tests cover negotiation/dependencies, empty setup/UC-route/history
requirements, attempts/backoff/deadlines/grants, provider reentry/close/latency,
UC-only observations and invalid ingress, and trusted startup cursor bounds.
Native plain/CTR/GCM tests inject two failed preparations, confirm UC from cursor
zero before any MC observation, perform a real third join, automatically return
through eight exact overlap positions with all commit replies dropped, deliver
forty more MC messages across key rotation, then confirm UC and close only the
owned membership while another forty UC messages remain deliverable. This is
staged IPv4 loopback qualification. The setup profile below covers opted-in native
connect admission; the profile below adds managed post-join loss/rejoin.
WAN/asymmetric delay, hostile peers, scale and playback continuity remain
separate qualification gates.

## Bounded recovery after a successful multicast join

`MembershipRecovery::Configuration.maximum_recoveries` defaults to zero, keeping
initial-join-only behavior. A nonzero value is explicit local policy from 1 to 8
and requires the same active `ReceiverBridge` passed to the membership factory.
Provisioned startup supplies its receiver automatically. Each loss episode uses
the existing 1..8 preparation-attempt budget, positive capped exponential backoff
and timeout of at most five seconds. The initial join plus at most eight recovery
episodes therefore permits at most 72 provider calls over this owner's lifetime.
Exhaustion, expiry or close cannot be bypassed by recreating the single-use owner.

`report_loss()` is a trusted local assertion about the currently joined membership
owned by this object. It immediately retires that exact ingress route and clears
its MC coverage evidence; repeated reports without another successful join are
refused. A fatal native receive/scheduler error on the same managed MC ingress
also records loss and removes that route instead of breaking the primary UC
runtime. Existing transient UDP-error handling is unchanged. A stopped primary
session, unowned route revocation/replacement, credential expiry/revocation,
root change, owner close and clock regression remain terminal. Unconfigured
members retain their previous error behavior. Source silence alone is not an OS
membership-loss signal; use the existing detector and host policy to decide
whether an explicit local loss report is appropriate.

The episode deadline starts at the recorded loss, including time before the next
pump, then is clipped by grant expiry. Snapshot projects pending loss immediately;
accounting/old-channel retirement occurs on pump. Counters distinguish total and
per-cycle attempts, losses, admitted recovery cycles and the current local route
epoch. Provider requests carry the cycle number and its original start/deadline.
Backoff begins from failed preparation completion and never extends that deadline.
Old receive-channel retirement runs outside owner/runtime locks, before a new
channel is started; no extra concurrent MC receive watch/worker is introduced.

Recovery waits for fresh correlated sender-confirmed UC and no pending return
operation before preparing a replacement membership. Membership loss does not
request or approve fallback and supplies no merge cursor. The host or separately
attached fallback controller must obtain approval using the existing trusted
cursor, source authorization, history/key, quota and bandwidth fences. Waiting
for approval consumes the original episode timeout. A failed or expired rejoin
leaves committed UC usable. Provider callbacks still run without owner, receiver,
protocol, runtime, channel or group locks and may inspect/reenter/close. Final UC
freshness/publication is serialized against local receiver-policy changes; late
or unauthorized prepared sockets are discarded within the same attempt budget.

The runtime retains its opaque membership identity, primary session, canonical
receive history, UC cache and all DATA high waters. Managed loss increments a
local route epoch and clears only MC path bits/timestamps in the exact-copy cache;
rejoin increments the epoch again. Old native binding tokens cannot admit DATA.
MC high waters are historical monotonic metadata, not current coverage. Actual
matching copies on the new route can refill coverage even after application pop;
only a strictly higher DATA position renews a high-water advancement timestamp.
Detector evidence resets on epoch change rather than carrying old recovery
hysteresis into the replacement membership.

`CoverageProof.route_epoch` and queued final-return jobs carry this local fence;
it is never encoded in control packets. Managed recovery refuses missing or old
proof epochs, and a final MC request posted or handed to the native queue before
loss is checked again before submission. Automatic-return operations are bound
to their original route epoch and cannot finish using another epoch's coverage.
A newly joined socket never directly changes source policy: returning to MC still
requires the existing fresh overlap revision, complete bounded coverage and sender
approval. Overlap commands or final returns already submitted before loss may
still commit remotely. This is not distributed cancellation or a guarantee of
uninterrupted playback; correlated queries, approved fallback and ordinary DATA
reliability remain necessary.

Components exercise opt-in bounds/same receiver, duplicate loss reports, UC
approval/freshness, canonical cache preservation, stale binding tokens,
per-cycle/lifetime budgets, original loss deadlines, callback latency/reentry and
close, native fatal receive handling versus unowned revocation, grant expiry,
MC-coverage invalidation and stale/posted/native-queued final returns. Native
plain/CTR/GCM tests start with a real successful join and confirmed MC, report
loss, wait for approved UC, inject two failed rejoin attempts while delivering UC,
perform a real rejoin, prove fresh eight-position overlap and deliver further MC
across key rotation while all commit replies are dropped. This is synthetic-
credential IPv4 loopback qualification, with no WAN/hostile-peer/scale claim.

## Provisioned native startup admission

`provision_startup(handle, StartupOptions)` is an explicit internal C++ selector
for a coordinated independent-source listener and an unopened dispatch Caller
group. Set the existing socket/group options first, then provision before listen
or connect. The immutable descriptor supplies the existing pairwise credential
callbacks and native policy, receiver/source policies, membership interface and
retry provider, a positive admission timeout <=5 seconds, and a poll interval of
1..50 milliseconds smaller than that timeout. Native exchange timeout cannot
exceed the admission timeout. Startup bit 128 and policy queries must both be
required; optional downgrade cannot activate this path. Defaults do not enable
it. Configuration is single-use and cannot be changed after opening. Group
member and accepted-child setup inherit the exact immutable descriptor under the
existing option/metadata locks. No numeric C socket option or credential lookup
service is added.

The profile deliberately starts with UC. After ordinary HSv5/KM setup, the Caller
installs only its established-peer UC dispatch route and freezes the complete
conclusions. It remains `CONNECTING`: public send/receive, peer-name and readiness
retain their normal unconnected behavior even if valid UC DATA has already
entered the core. The separate hybrid exchange must authenticate and select the
required startup capability. A receiver and membership owner then capture a
verified empty initial receive origin before any membership attempt or fallback
request. Because application receive is still unavailable, that original local
merge cursor remains valid if UC DATA arrives before publication. It is not a
reported sender progress horizon and causes no manufactured ACK/drop/cursor
advance.

The sender derives stream/member/origin from its actual coordinator binding and
freezes the authorization lease. One deferred coordinator object and lease are
retained while enrollment is busy, rather than reallocating on every poll.
Enrollment still takes all existing authorization/runtime/history/key/budget
fences and grants no policy while pending. The Caller posts one bounded explicit
fallback operation at its validated initial origin, including while source
activity is paused or no DATA has yet been produced. The sender may deny it under
its actual UC recipient/bandwidth limits. A fresh correlated commit or query
result must confirm UC before the Caller publishes `CONNECTED`. Source accept
remains ordinary primary-handshake completion; it is not a remote receipt that
the Caller has published its connection.

Publication rechecks owner/native lifetime, grant, receiver freshness, confirmed
UC, absence of a pending return, original deadline and exact runtime/socket owner.
It is serialized with profile and socket close. Only then are group readiness and
normal connect completion/callbacks published. Dropped commits reconcile with
queries; an observed or guessed remote policy is insufficient. Missing provision,
unsupported required capability, bad credentials, denial, exhausted/unknown
reconciliation, close or expiry never publishes the Caller as connected.

Both asynchronous Caller establishment and blocking waits keep the original
connect deadline. Admission is clipped by that deadline and the explicit startup
budget; authentication, queueing, credential work, UC reconciliation and lock
acquisition consume it. The asynchronous actor reuses its retry timer for pending
admission rather than completing the handshake callback early or blocking a
protocol shard. Timeout retains the usual timeout result. Denied UC capacity is
a connection rejection; failed authentication/unconfirmed setup is a setup
security failure. Failure cancels the local owner, retires its exact UC route and
closes the unpublished runtime. Expected-runtime retirement cannot remove a
replacement route. Late replies cannot revive a failed or closed Caller.

One fixed setup owner below 2 KiB reserves one additional scheduler service per
provisioned member, alongside the existing native service. It drives the existing
source/receiver owners and their bounded queues; it adds no DATA queue or thread.
Membership success uses the ordinary receive-channel resources described above.
Credential callbacks and native join preparation run without setup-owner,
socket/group/runtime or native queue locks, under their existing nonblocking
contracts. Callbacks must not wait on network I/O, synchronous connection I/O or
cleanup. Use weak back references. There is no production credential discovery,
key generation policy or implicit trust in a packet source.

After publication, the setup deadline stops being a connection lifetime. The
owner continues control pumping and bounded membership retries; failed joins or
exhausted membership attempts leave approved UC usable. Joining does not change
source delivery policy. This profile does not attach automatic fallback/return
controllers; an advanced host may obtain the receiver/membership owners after
publication and attach separately configured controllers when their negotiated
features and reporting contracts permit it. `StartupOptions.membership.interface`
is the explicit local interface used for these joins. Existing coordinated
restrictions still apply, including rejection of primary session authentication
and primary ACP1 on that profile; auxiliary authenticated hybrid control remains
separate. Plaintext/CTR/GCM data profiles retain their established limits.

Socket teardown cancels the setup owner before runtime/member retirement,
releases its service, revokes owned MC observations/routes and retires only its
own UC dispatch alias. A terminal control failure after publication does not
retroactively undo a completed connection or roll back committed UC. Already
submitted control or DATA cannot be recalled: a lost/late request may have
committed remotely even if local setup returns failure. There is no distributed
abort or universal remote commit cutoff; source grant, budget and transport
lifecycle fences remain authoritative. Control grant expiry is not a new
cryptographic DATA revocation mechanism.

Native cases cover actual group connect with every commit reply dropped across
plain/CTR/GCM, blocked public I/O during enrollment, UC delivery despite failed
joins and live control after the admission deadline. An asynchronous encrypted
case performs a real join after publication. Failure cases cover absent source
provision, rejected/wrong credentials, the fifth UC recipient against a four-member
budget, fully lost replies despite a committed sender policy, buffered DATA that
remains publicly unreadable/unready before publication, close, original connect
timeout, and unchanged unprovisioned join failure. These are synthetic-credential
IPv4 loopback tests. The managed-loss profile above can be enabled through
`StartupOptions.membership.maximum_recoveries`. A public numeric selector,
production provisioning, hostile-peer/scale/WAN timing and playback-continuity
qualification remain future work.

## Repeatable local network and timing qualification

Enabled builds add a separate `robotweax_srt_hybrid_network_tests` CTest gate,
selected by `--include hybrid_network_`. It has a 30-second aggregate bound and
`RUN_SERIAL`; it is excluded from the general C++ gate and the existing multicast
partition, whose timeouts are unchanged. Pure `hybrid_link_` cases run in both
default and enabled builds. No production source, wire protocol, feature default,
credential admission rule or public API changes are introduced by this gate.

The test-local link model uses fixed 64-datagram queues per direction/path, each
holding at most 1,500 bytes per datagram. Test workers forward queued datagrams
through the original native UDP sockets, preserving packet bytes, authentication
tags, destination endpoints and source ports. A successful modeled send means the
emulated link accepted the packet; later queue loss models network loss rather
than an actual kernel send failure. Deterministic alternating delays induce
reordering, selected sends duplicate, periodic network loss and an initial burst
drop exercise reconciliation, and selected attempts return EAGAIN without
submission. Queue pressure returns retry without partial duplicate admission.
This is a test harness, not a production transport queue or a physical/WAN link.

The integrated profile applies source auxiliary-control delays of 8/24 ms and
Caller delays of 1/3 ms, with periodic loss/duplication and retryable sends in both
directions. UC DATA uses 4/10 ms and MC DATA 2/8 ms, also with periodic loss and
duplication. Primary handshake, KM, ACK and NAK packets pass unchanged. The
returned report ages therefore still exclude the emulated link transit, as on a
real unsynchronized path; this profile makes no claim about arbitrary transit
beyond the chosen control/freshness budgets. Scheduling lateness is measured and
logged rather than asserted to have a universal bound.

For each of plaintext, CTR and GCM, the same provisioned connection performs
startup UC admission under impaired source control, a real membership join,
fresh overlap/MC return, a DATA burst with recoverable gaps, local membership
loss, sender-approved UC, two failed preparation attempts and real rejoin,
another exact overlap/return, then native source failure after fresh UC approval
and explicit atomic UC-only source recovery. Every commit reply is dropped;
correlated queries reconcile the actual revisions. Each profile receives 177
canonical messages across several key rotations. Per-link JSON records include
accepted/drop/duplicate/retry counts, observed reorder counts, peak queue use and
maximum worker scheduling lateness. Healthy-path socket I/O errors and queue
overflows must remain zero.

Overlap proof is collected from validated receive-path observations before
waiting for application playout; a 200 ms playout delay must not be charged to a
200 ms proof lifetime. MC network loss is disabled for the exact eight-position
proof interval, while delay, reordering, duplication and retry still apply.
Other DATA bursts retain periodic loss and end with two un-dropped witness
packets (or the sole packet for a one-packet burst). These make earlier missing
sequences observable to the ordinary reliability path. A lost isolated final
packet has no later DATA horizon from which to detect the gap: this gate does
not claim isolated tail-loss recovery or infer an ACK horizon from control
progress. No DATA/ACK/drop semantics are changed to make the tests pass.

Two negative profiles queue policy replies for 250 ms, close startup after the
source has committed UC, then actually forward those replies and check that the
Caller never publishes or joins. Separately, 200 ms exchange delay outlasts an
original 120 ms connect deadline; late exchange datagrams cannot revive the
failed connection. The model never rewrites the echoed request reference, native
root, policy revision or deadline. Close/timeout checks remain separate from
whether an already accepted remote command committed.

These results qualify only this controlled Darwin IPv4 loopback fault profile
with synthetic credentials. They do not qualify hostile peers, physical/WAN or
asymmetric routing, arbitrary jitter/RTT, isolated tail loss, delayed/reordered KM,
sustained overload/fairness, large member counts, other platforms or uninterrupted
playback. Next qualification should expand the declared latency/traffic envelope
and loss observability deliberately before public provisioning or release claims.

### Multiple receivers under selective control loss

The same serial network gate also runs two multi-receiver scenarios across
plaintext, CTR and GCM. The source-side test link can select one Caller endpoint
for auxiliary-control impairment while forwarding other endpoints' non-commit
control directly. This filtering is test-local and preserves the original UDP
endpoints and packet contents. All commit replies remain deliberately dropped,
so healthy receivers also reconcile transitions through correlated queries.
Primary handshake, KM, ACK and NAK handling is unchanged. No production policy,
wire field, quota, scheduler or credential rule is modified by these tests.

The first scenario has three live DATA receivers: two on approved UC and one on
confirmed MC. One UC receiver's auxiliary replies are blackholed until its policy
confirmation becomes stale. The other two receivers retain fresh control while
all three consume the same stream through periodic DATA loss, reordering,
duplication and retryable sends. The affected receiver then reports membership
loss; no new preparation attempt occurs while UC confirmation is stale. Restoring
its replies permits fresh confirmation, two failed preparations and a real rejoin.
Each receiver consumes 168 canonical messages across key rotations. The two
unaffected receivers keep their runtime identities, policy modes/revisions and
membership epochs; no member detaches. This qualifies that particular selective
fault sequence, not adversarial-member or scheduler-fairness isolation.

The second scenario fills the existing four-member UC admission quota. One
receiver's stale control does not release its committed UC lease: an additional
Caller fails admission and never joins. The four established receivers continue
receiving DATA. A healthy receiver then completes exact eight-position overlap
and return to MC. Only after its real UC reservation drains does a fresh Caller
successfully join at absolute stream position 48. This late receiver has its own
new root and confirmed initial UC admission, and its sender key state is ready
before more DATA is produced. A further Caller is rejected when the four UC
reservations are occupied again. The four original receivers each consume 88
canonical messages, and the late receiver consumes positions 48 through 87: 392
successful application deliveries per encryption profile. The selected receiver
remains stale, while the other four receivers retain fresh control and the
expected UC/MC modes. Rejected Callers record no membership preparation.

`HYBRID_MULTI` JSON records identify scenario/profile, source and received message
counts, DATA receiver count, healthy-control checks, selectively dropped replies,
bypassed control and quota/recovery outcomes. Healthy-path link I/O errors and
queue overflows must remain zero. Synthetic grants are explicitly 10 seconds for
these multi-receiver fixtures; operation/retry and membership deadlines retain
their existing bounds. DATA bursts retain the earlier un-dropped tail witnesses,
and proof observations precede application playout. No isolated tail-loss or
delayed/reordered-KM claim is added.

This is controlled Darwin IPv4 loopback evidence for three or five live DATA
receivers with at most four UC leases, not general member-scale, overload,
hostile-peer, physical/WAN, cross-platform or uninterrupted-playback qualification.
The same synthetic credentials/shared group keys retain their existing security
limits. Larger populations, stronger traffic contention and delayed KM remain
separate qualification work.

### Bounded runtime key-message timing

Enabled builds also register `robotweax_srt_hybrid_key_timing_tests`, selected by
`--include hybrid_key_network_`. Its independent 30-second `RUN_SERIAL` budget
keeps the earlier network and multicast partitions unchanged. Two cases run
CTR and GCM through native IPv4 loopback sockets. They install test hooks after
the initial encrypted connection succeeds; initial handshake key material is
outside this timing model. No production crypto, protocol or policy changes
are needed. The earlier delayed-policy/close case now waits for an actual
non-dropped reply to enter the test queue before closing its Caller. A committed
UC reservation alone can become visible before reply enqueue; stage diagnostics
identify admission, queue and post-close delivery waits. Delays and deadlines
remain unchanged.

The test wrapper delegates HC and DATA to the existing asymmetric fault links.
Only runtime KMREQ/KMRSP packets enter another fixed 64-datagram, 1500-byte queue,
with at most 16 releases per 1ms worker slice. It forwards the original bytes
from the original UDP sockets. The request link alternates modeled 40ms and 2ms
delays and duplicates every second accepted request; retries can overtake older
copies. Responses alternate 10ms and 2ms delays, duplicate every second reply
and lose the first runtime reply. Most DATA is paced at 10ms per message to
keep this delay below the configured key-announcement lead; the existing
8-message overlap proof bursts retain their 3ms pacing. This is one bounded
timing envelope, not arbitrary latency or jitter tolerance.

The first case consumes 112 canonical messages per crypto profile through UC,
exact overlap/MC, local membership loss, confirmed UC, real rejoin and another
exact return to MC. After a later key has carried DATA, it replays the first
captured runtime KMREQ within the existing four-entry material history and
waits for its echoed response. Continued exact plaintext delivery and unique
packet counts check that replay did not roll the receiver back. Duplicate and
reordered request counters and a dropped response are required to be nonzero.

The second case consumes 96 canonical messages per profile. It acknowledges the
first rotation, retains current KMRSP packets for the next generation, and sends
DATA across that generation's switch while the sender remains `securing` and
the receiver stays `secured`. Replaying the acknowledged older KMRSP must leave
the newer rotation pending and its requests retrying. Releasing the captured
matching response restores `secured`; subsequent application reads must match
every expected message. Held replies use one fixed latest-packet capture, so
retries cannot grow an unbounded backlog. These captures are never logged.

`HYBRID_KEY` JSON records report scenario/profile, delivered message count,
consecutive request/response material changes, duplication/drop/reordering,
replays and held-response counts. Material changes count consecutive payload
changes, including an echoed historical request; they are not unique key counts.
Each healthy link checks zero I/O errors and queue overflows. Grants remain
10 seconds; operation/recovery bounds, two un-dropped DATA tail witnesses and
proof-before-playout requirements are unchanged.

Runtime KM control remains the ordinary passphrase-based unauthenticated path;
these replay checks exercise bounded duplicate histories and matching pending
responses, not hostile-peer authentication or arbitrary-age replay protection.
Those single-receiver cases do not qualify keys arriving after their usable
DATA horizon, initial KM loss, multi-receiver KM contention, sustained overload,
physical/WAN paths,
other platforms or uninterrupted playback. Shared-credential, no-forward-secrecy
and primary coordinated-source SESSIONAUTH/ACP1 restrictions still apply.

### Multiple receivers during bounded key-message delay

The same serial key-timing gate adds two cases selected by
`--include hybrid_key_network_multi_receiver`. Together with the two preceding
cases, the gate retains its 30-second aggregate budget. Both new cases use CTR
and GCM with three native DATA receivers and two committed UC members. They
exercise selective KM response delay and late admission at modest offered load;
they are not a general key-distribution contention or member-scale qualification.

The source KM wrapper optionally selects one immutable receiver endpoint.
Runtime KM for other endpoints bypasses that wrapper unchanged and is counted;
only the selected peer enters its fixed queue/captures. Its requests alternate
modeled 20ms/2ms delays and duplicate every accepted request. The selected Caller
has a separate response wrapper, initially delayed 1ms, then explicitly held
across one key switch. Only the latest held response is retained. Other Callers
receive normal KM, including the late Caller's initial handshake. Original bytes,
UDP endpoints, group keys, admission rules and crypto implementation are unchanged.

These new cases keep all commit replies dropped, but apply no additional
impairment to other auxiliary control, so correlated status queries can maintain
fresh confirmations.
The earlier combined-loss network cases retain their separate auxiliary fault
patterns. An initial attempt with that global periodic auxiliary-loss pattern
observed a UC confirmation expire at DATA position 37, before response holding
began; it is outside the isolation claim here. Freshness assertions remain active
for every receiver, and operation/confirmation deadlines are unchanged. DATA
retains the earlier delay/loss/duplication/reordering/retry model, 10ms producer
pacing, two un-dropped tail witnesses and exact proof before application playout.

The first case starts two UC receivers and one confirmed MC receiver. After
acknowledging the selected peer's first rotation, it holds the next matching
response. All three receivers must receive and read DATA through position 67
while that source member remains `securing`; both healthy source members must
be `secured`. Replaying an older acknowledged response leaves the selected
rotation pending through at least two more KM attempts. Healthy UC/MC runtime
identities, policy revisions/modes and membership epochs stay unchanged. Releasing
the matching response restores `secured`, then all three receive through position
95: 288 canonical application deliveries per profile.

The second case starts one UC receiver and one confirmed MC receiver. While the
UC member's next response is held and its sender remains `securing`, a fresh
Caller joins at the current observed stream position, between 40 and 63. It
completes its own handshake/root, confirmed UC admission, real membership join
and sender key readiness. Both old receivers and the newcomer then receive and
read through position 67 before the selected response is released. The healthy
MC receiver retains its runtime, revision, mode and route epoch; the newcomer
is `secured` across the DATA key switch. After the release, originals consume
0..95 and the newcomer consumes its actual origin through 95. The delivery total
is `288 - late_origin`; no historical DATA is invented for the new receiver.

`HYBRID_KEY_MULTI` JSON records identify scenario/profile, source/received counts,
three DATA receivers, observed late origin, held replies, historical response
replays, selected-request duplicates, bypassed KM and healthy-control checks.
All healthy links require zero I/O errors and queue overflows, every receiver
requires exact canonical reads and unique packet counts, and all three sender
key states finish `secured`. Grants remain 10 seconds. No production, public C
ABI, wire, capability, provisioning, scheduler or quota change is introduced.

This is controlled Darwin IPv4 loopback evidence for three authorized receivers
and one pending KM generation at a time. It does not cover simultaneous delayed
KM for many members, prolonged delay beyond the usable key horizon, combined
auxiliary-loss stress in these new cases, arbitrary-age replay, hostile-peer
isolation, sustained overload, physical/WAN paths, other platforms or uninterrupted
playback. Ordinary unauthenticated runtime KM and the earlier credential,
forward-secrecy and primary SESSIONAUTH/ACP1 restrictions retain their limits.

### Bounded packet-load and delay matrix

Enabled builds register `robotweax_srt_hybrid_load_tests`, selected by
`--include hybrid_load_network_`, with a separate 30-second `RUN_SERIAL` budget.
One CTR and one GCM case each run the following three rows. Existing network
and key-timing gates retain their budgets. This is a finite offered-load matrix
on Darwin IPv4 loopback, not a sustained-throughput or physical RTT benchmark.

| Row | Target packets/s | Payload bytes/s at target | DATA delay (ms) | Selected KM request delay (ms) | Selected KM response delay (ms) | Source/Caller auxiliary delay (ms) | Source UC DATA capacity policy |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | 100 | 131,600 | 5 / 10 | 8 / 16 | 4 / 8 | 4 / 8 and 2 / 4 | No capacity retries |
| 1 | 250 | 329,000 | 15 / 25 | 8 / 16 | 8 / 16 | 8 / 16 and 4 / 8 | No capacity retries |
| 2 | 500 | 658,000 | 30 / 50 | 10 / 20 | 10 / 20 | 12 / 24 and 8 / 16 | Retryable capacity refusals permitted and counted |

Each delay pair alternates deterministically between its listed values; it is
not a random jitter distribution. DATA additionally drops every 17th accepted
send, duplicates every seventh and returns a configured retry every 43rd
attempt. Burst tails retain two un-dropped witnesses. Selected runtime KM
duplicates every third accepted request/response, without configured KM loss.
All commit replies are dropped; other auxiliary control has only the listed
delays, applied to both Caller channels and the shared Source channel. ACK/NAK,
initial handshake KM and the other receiver's runtime KM are unchanged. The
fixed link queues remain 64 datagrams of at most 1500 bytes, with bounded worker
slices. No production transport, crypto, policy or wire changes are introduced.

The fixture uses a 4,000,000 bytes/s Source maximum-bandwidth setting and two
receivers. One stays on UC; the other starts on confirmed MC. After eight small
bootstrap/proof messages, the producer sends 96 canonical 1,316-byte messages,
then pauses to report MC membership loss. The affected receiver must obtain
fresh UC confirmation within 600ms, fail one preparation and rejoin within 1s
from loss, then return through a new exact eight-position overlap proof. The
complete return, including application reads, must finish within a 600ms test
budget. The proof retains the row's DATA delay/duplication/retry settings, with
loss disabled for its exact-coverage window. A final 64 large messages follow.
Each receiver must read all 176 canonical messages exactly, including 160 large
payloads, with matching unique-packet counts. Both sender key states finish
secured, one UC member remains, Source failure stays false and every observed
policy confirmation remains fresh. Existing operation/confirmation/recovery
settings and 10-second synthetic grants are unchanged.

Absolute producer deadlines retain phase through timer jitter. If the producer
falls more than one interval behind, it resets the deadline rather than building
an unbounded catch-up burst. The log reports resets and the actual accepted
application rate over the two loaded bursts, excluding control/proof pauses.
Actual rate must reach at least 75% of the target. This is offered application
rate, not measured receiver goodput or a production capacity guarantee. Relative
sleeps in the first trial undershot the faster targets; the rate threshold was
retained when the test pacer was corrected.

A test-only observer records changed runtime KM submission times and the first
forward sequence carrying a different DATA key selector on the selected UC
channel. It ignores retransmissions/older sequences and uses fixed eight-entry
arrays. Five announcements and five DATA key switches are required. The minimum
Source submission lead is measured directly. The request budget margin adds the
minimum programmed DATA delay, then subtracts maximum programmed KM request
delay and observed request-worker lateness. It must stay positive. The ACK budget
margin subtracts maximum programmed request/response delays and both workers'
lateness; it may be negative. These are model budgets, not measurements of peer
key installation or ACK receipt, and exclude unmodeled socket/host processing.
Exact decrypted application delivery remains the functional check.

The highest row probes the fixed unicast DATA queue boundary. A diagnostic run
observed 15 retryable full-queue refusals at a 64-slot peak while earlier delivery,
key-readiness and recovery checks had passed. This queue carries UC originals
and unicast repairs for both receivers. Full-queue sends are unaccepted and
return `would_block`; accepted packets are not silently discarded and storage
is not enlarged. Row 2 permits and reports these Source DATA capacity retries.
Rows 0/1 and all other DATA, control and KM queues still require zero capacity
refusals. Every row requires zero UDP I/O errors and queue peaks within 64 slots.
This separates a capacity-boundary result from an unsaturated operating claim.

`HYBRID_LOAD` JSON rows identify crypto/profile, target and actual offered rate,
pacer resets, payload/message counts, programmed delay maxima, five switches,
minimum announcement lead, request/ACK budget margins, fallback/rejoin/return
elapsed times, UC/MC queue peaks and Source DATA capacity retries. Failed early
trials remain in the qualification archive. No new capability, public C option,
provisioning API, scheduler/quota rule or primary SESSIONAUTH/ACP1 support is added.

This matrix qualifies only these finite two-receiver bursts and deterministic
fault patterns. It does not establish sustained overload stability, fairness,
large member scale, arbitrary loss/jitter, real WAN/asymmetric RTT behavior,
keys beyond their usable DATA horizon, isolated lost-tail recovery, hostile-peer
isolation, other-platform results or uninterrupted playback. The existing
unauthenticated runtime KM, credential and forward-secrecy limits remain.

### Upper-profile soak qualification

`robotweax_srt_hybrid_soak_tests` adds two HC-conditional native cases selected by
`--include hybrid_soak_network_`. Its 30-second `RUN_SERIAL` CTest gate explicitly
sets `ROBOTWEAX_TEST_HYBRID_SOAK_SECONDS=4`, keeping CI on a short smoke run even
if the parent environment requests a longer soak. Existing gate budgets remain
unchanged. For a reproducible longer run, use a new output directory:

```sh
./tools/python tools/qualify_hybrid_soak.py \
  --build build-hybrid-control --seconds 30 --output /tmp/hybrid-soak-30s
```

The driver defaults to 30 loaded seconds per CTR/GCM profile and accepts 4..300.
Only the explicitly executed duration is qualified. It requires HC, ACP1 and
AEAD enabled, checks that both native cases fully pass with no skips, validates
both metric records and rejects short duration, deficient offered rate, incomplete
DATA/rotation counts or exceeded retention/recovery budgets. It records commit,
dirty-worktree status, backend, platform, executable/cache/log SHA-256 values,
command and metrics in `qualification.json`; failures retain a failed record and
raw `native.log`. Existing output directories are refused. Four Python regressions
exercise valid capacity pressure and rejection of incomplete or invalid evidence.

The soak uses the preceding highest profile: target 500 packets/s, 1,316-byte
payloads, Source maximum bandwidth 4,000,000 bytes/s, DATA delay 30/50ms with
periodic loss/duplication/retry, selected UC KM request/response delay 10/20ms and
auxiliary delay on both Caller channels. Commits remain dropped; ACK/NAK, initial
handshake KM and the other member's runtime KM remain unchanged. Source DATA
capacity refusals are permitted and reported at the fixed 64-slot boundary;
all other DATA, control and KM queues require zero capacity refusals, and all
UDP I/O errors must remain zero. This is an offered application rate, not
receiver goodput or a full RTT/network-capacity benchmark.

There are two long loaded phases, each containing half the requested DATA count,
with one deliberate recovery pause between them. Concurrent readers validate
every payload while the producer runs, avoiding periodic application-drain
pauses within a phase. The absolute pacer retains the preceding bounded catch-up
policy. Loaded time excludes bootstrap, drain and recovery; total loaded messages
are `seconds * 500`, actual rate must reach 375 packets/s, and both receivers must
read that count plus 16 small proof messages exactly. The payload now carries a
full 64-bit position, preventing the earlier pattern's 256-message alias. The
same factory is used by the short load matrix.

At the midpoint, the MC receiver reports membership loss, obtains fresh UC within
600ms, fails one preparation, rejoins with a new route epoch within 1s from loss,
and returns through an exact eight-position overlap proof within the existing
600ms test budget. Proof retains the profile's delay/duplication/retry with loss
disabled for exact coverage; loaded-phase tails retain two un-dropped witnesses.
Every monitored confirmation remains fresh, Source failure stays false, one UC
member remains and both sender key states finish secured. Protocol operation,
confirmation and recovery timeouts are unchanged. Synthetic grants are explicitly
`ceil(requested seconds / 0.75) + 15` for the Source and every Caller, covering
the permitted pacing overrun; the original fixture's
default ten-second grants remain unchanged in other cases. A first long trial
correctly closed the later Caller at its still-ten-second grant; the fixture now
applies the chosen grant lifetime consistently rather than relaxing expiry checks.

The test observer counts key switches/material changes without retaining a
per-rotation history; its fixed eight-entry timing history remains available to
the short matrix. Exact switch count is `(loaded_messages + 15) / 32` using integer
division. Sender retention is sampled every 125 produced packets and after each
phase. Each sampled per-member send buffer must stay within a 512-packet test
budget, and both must drain to at most 16 packets at completion. These are sampled
packet gauges, not a whole-process memory bound or proof that no intermediate
peak occurred. Fixed link queues retain 64 datagrams/1500 bytes and bounded worker
slices. Storage for readers, payload identities and counters stays constant with stream length.

`HYBRID_SOAK_SAMPLE` records show progress, sampled retained DATA and Source DATA
capacity refusals. `HYBRID_SOAK` records identify duration, loaded count/time,
actual rate, two exact receivers, switches/material changes, samples/pacer resets,
retained peaks/final values, link queue peaks/retries and recovery timings. The
qualification archive distinguishes smoke, failed initial-grant trial and clean
long-run results. No production protocol, crypto, public C ABI/options, capability,
provisioning, scheduler/quota or primary SESSIONAUTH/ACP1 support is changed.

The executed long run is still controlled Darwin IPv4 loopback with two authorized
receivers, deterministic faults and one recovery pause. It does not establish
hours-long stability, overload/fairness at arbitrary scale, arbitrary jitter/loss,
real WAN/asymmetric RTT behavior, isolated lost-tail recovery, hostile-peer
isolation, other platforms or uninterrupted playback. Shared-credential,
unauthenticated runtime KM and forward-secrecy limitations remain.

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
| 64 | 8 | Multicast next; negotiated delivery-response effective boundary; return-ready coverage begin |
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
activity. FALLBACK_REQUEST selects unicast; commits have a nonzero revision.
Overlap and ready messages require negotiated overlap support. With bit 64,
MULTICAST_READY selects overlap or multicast as described below; without bit 64
it retains legacy parsing and has no return side effects in the sender bridge.

STREAM_PROGRESS distinguishes production from actual original-submission high
water and multicast-submission high water. A high water mark is not proof of a
contiguous submitted prefix or an ACK horizon. Retention can advance beyond an
older emitted position. The optional producer uses actual UDP outcomes; codec validity alone cannot
establish truth or delivery. PATH_REPORT
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
