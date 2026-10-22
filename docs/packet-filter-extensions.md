# Packet-filter extension contracts

The optional source-tree component in
[`experimental/packet_filters`](../experimental/packet_filters/README.md)
defines private C++ contracts for future custom packet filters. Enable it with
`ROBOTWEAX_SRT_BUILD_FILTER_CONTRACTS=ON`; the default is OFF. It is not installed
or linked into the transport library and changes no socket or wire behavior.
It is independent of congestion-controller selection.

The processing scope is a logical stream or a member transport. Receiver
ownership is separate from processing scope: shared encoder/source state and
receiver-specific decoder state can have independent lifetimes. Logical and
member sequences are distinct, and input before path assignment needs no
member origin. Core-owned path budgets can differ, paths and receivers carry
generation identities, and feedback may be absent, optional or required.
These choices preserve room for aggregation bonding and multicast; they do not
implement either mode.

An owner admits immutable context/membership snapshots and supplies bounded
state, output metadata and byte storage. The dispatcher copies the snapshot,
serializes one instance, contains callback failures and validates a complete
batch before exposing any output. Failed admission leaves the previous snapshot
active. Membership events can retire paths/receivers while retaining shared
logical state. An instance whose own member or receiver departed must be closed.
Queued work must be checked again against current identity, protection and
capacity before submission.

Callbacks are trusted native code. They must not block, reenter transport APIs,
allocate during events or retain borrowed references. Separate instances can
run concurrently; synchronized ownership can move between threads. Declared
storage and output limits do not sandbox native memory access, CPU use or
amplification over time. Batch publication does not roll back provider state:
reserve queue/rate capacity before dispatch, or fail terminally on commit error.

The future coordinator still owns routing, scheduling, aggregate rate/queue
admission, retention, logical receipts, deduplication, deadlines, receiver
feedback suppression and protection. Recovered candidates need core validation,
including authentication when required by their profile. Cleartext input is not
authenticated. Protection identities enforce domain separation at this boundary;
the component performs no cryptography.

Contract tests cover unequal path budgets, independent sequence domains, path
and receiver churn, optional/absent feedback, recovery with no live paths, stale
contexts, protection domains, storage overlap, overflow, atomic output validation,
terminal callback failures and concurrent decoder handoff. The prototype's
64-path/256-receiver snapshot ceilings are storage bounds, not wire limits or
stable ABI commitments. No public provider API or new runtime filter is enabled.
