# Packet-filter extension contracts

Private, source-tree-only contracts for future packet-filter bindings. This
contract component defines no wire format, provider registry, socket option,
recovery algorithm or transport scheduler. The separate default-off
[member binding](../../docs/experimental-filters.md) supplies a bounded registry
and individual-socket runtime adapter. It is independent of congestion controllers.
The standalone contract target is not linked into `robotweax_srt` or installed.
The optional member binding compiles the contracts into the transport library.

Build with the separate default-off `ROBOTWEAX_SRT_BUILD_FILTER_CONTRACTS` flag:

```sh
cmake -S . -B build-filter-contracts -DCMAKE_BUILD_TYPE=Debug \
  -DROBOTWEAX_SRT_BUILD_FILTER_CONTRACTS=ON
cmake --build build-filter-contracts --target robotweax_srt_filter_contract_tests
ctest --test-dir build-filter-contracts -R filter_contract --output-on-failure
```

## Group-independent boundary

`filter.hpp` validates inputs and complete output batches against core-owned
contexts, ownership, paths, receivers and resource limits. It does not define
a recovery algorithm, wire format, network feedback protocol, crypto profile,
or scheduler. Filter inputs/outputs borrow payloads for the duration of the
call. The owner must preserve committed output in its own bounded storage and
apply the binding's admission and protection policy before providing input.
Cleartext bindings do not authenticate their inputs. Recovered candidates must
pass the core's validation, including authentication where the profile requires
it, before delivery.

The processing scope is member transport or logical stream; receiver ownership
is independent of that scope. Logical-stream inputs can originate from different
paths, with their member sequence recorded only as provenance. Application
symbols before path assignment have no origin. Core lifting/epoch handling
maps wrapping wire sequences to the bounded positions in the environment.

Logical recovery may target another eligible path or leave routing to the core.
It may also deliver already-recovered data while no network paths remain.
Member-local filters require their original active member generation. Session,
stream, direction and configuration changes reject stale work. A receiver can
leave or change generation independently of shared source state. Feedback-free
and feedback-required processing are distinct capabilities.

Bounds cover per-batch output count/bytes and total declared filter state,
including receiver multiplication without integer overflow. The environment
contains independently computed path payload budgets; the payload-budget helper
subtracts group, crypto and filter-stack overhead without overflowing. Caller
limits and environment snapshots are trusted core policy, never peer-supplied
admission authority. State/output declarations do not sandbox native code.

The owner remains responsible for actual storage enforcement, scheduling/rate
budgets across batches, crypto/protection domains, receiver feedback suppression,
logical receipts, retention, deduplication and delivery deadlines. The checks
must run before committing any batch. A malformed output must not change any
of those core responsibilities.

Admit each immutable membership snapshot before using its packet checks. Full
duplicate/identity admission runs on membership changes; shared-stream packet
processing does not scan the entire receiver set. Specific receiver lookup is
still a linear bounded lookup in this prototype. Snapshot lifetime and
serialization belong to the owner.

Protection domains carry opaque core-issued key/epoch identities. Protected
symbols can only target admitted paths/receivers in the same protection context;
logical payloads can be protected later under the destination's context. These
checks validate domain separation, not cryptographic operations or peer trust.

Contract tests cover member-local recovery, logical XOR repair across unequal
paths with independent member sequences, out-of-order inputs, member churn,
receiver churn, feedback-free operation, stale context rejection, and resource
limits. They do not qualify runtime aggregation, multicast, encrypted custom
filters, or a public extension API.

## Filter lifecycle and output storage

`filter_dispatch.hpp` adds a serialized callback instance using owner-supplied
state, output metadata and byte storage. Setup reserves the declared base state
plus the maximum admitted receiver capacity, checking overflow, alignment and
overlapping buffers before invoking construction. The prototype copies current
membership into bounded snapshots (up to 64 paths and 256 receivers); it does
not borrow the caller's membership containers between callbacks.

Events distinguish input, clock ticks and environment changes. Environment
changes provide both snapshots so a filter can retire departed receiver slots
or paths without losing shared stream state. A member's generation change does
not reset a logical filter. Epoch, configuration, direction or protection
changes require a new instance. Retention can advance through the environment's
sequence window. The filter owns its bounded receiver-to-state-slot mapping;
the coordinator owns receiver admission and retirement decisions.

The output writer copies bytes into supplied storage and latches capacity
errors, even if a callback ignores them. The complete batch is validated before
any outputs are returned. An invalid output, callback error, exception or
backwards clock fails the instance permanently and clears the proposed batch.
Bad core inputs or inadmissible snapshots are rejected before callbacks and
leave the active state unchanged. Output views last until the next dispatch,
environment update or close. Close is terminal and idempotent, destroys a
successfully constructed state once, and clears used storage. Buffers and
callback code must outlive the instance; failed construction must roll back its
own partial state.

Batch publication does not make provider state transactional. A callback may
already have advanced its state when the coordinator discovers insufficient
queue or rate capacity. Reserve before dispatch, or make a failed commit terminal;
do not retry the input against the advanced state. Queued output retains its
context and explicit destination/receiver generations. The coordinator checks
those identities, protection and path capacity again before submission rather
than treating an earlier validation as permission to use a replaced path.
An inadmissible membership update leaves the prior snapshot intact. Close an
instance whose own member or receiver has departed; shared logical state may
continue through a separately admitted update.

Callbacks are trusted native code and must not retain references, block,
allocate on event paths or reenter transport APIs. Storage limits cannot prevent
arbitrary native memory access or enforce a callback CPU-time limit. The
same callback implementation may run concurrently for different instances.
Construction, events and destruction have no fixed thread affinity: the owner
can hand an instance between threads with synchronization. Instance-owned state
must not be replaced by unsynchronized provider globals or thread-local state.
The coordinator must separately reserve bounded queue/rate capacity and atomically
commit or reject the complete validated batch; per-callback limits alone do not
bound repair amplification across time. This dispatcher does not negotiate a
filter, send network traffic, authorize recovered data, deduplicate logical
delivery, or authenticate feedback.

The 64-path and 256-receiver snapshot ceilings are private prototype storage
bounds. They are neither wire limits nor a stable ABI commitment. Future group
and multicast coordinators still need their own routing, aggregate admission,
logical receipt, membership and protection design.
