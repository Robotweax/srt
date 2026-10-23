# Experimental compiled packet filters

Development feature, default OFF. Enable with
`ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_FILTERS=ON`. Compiled filter names are selected
through the existing pre-connection `SRTO_PACKETFILTER` string option. Built-in
FEC parsing, negotiation and encoding retain their direct implementation.

The initial runtime binding supports individual Live Message Caller/Listener
sockets with TSBPD, the built-in Live controller, and cleartext. Encryption,
session authentication, Rendezvous, groups and custom congestion controllers
are rejected for this binding. This is a bilateral experimental wire profile;
it is not Haivision packet-filter plugin interoperability.

## Build and use

```sh
cmake -S . -B build-filters -DCMAKE_BUILD_TYPE=Debug \
  -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_FILTERS=ON
cmake --build build-filters --parallel
ctest --test-dir build-filters --output-on-failure
```

The bundled source exposes two independently selected providers. `rwx-repeat-v1`
sends a complete copy of each
original DATA packet as repair and recovers a missing original from that copy.
This deliberately simple example exercises the extension boundary; it adds
approximately 100% repair traffic plus framing, and is not an efficient FEC
algorithm or a performance recommendation.

Set the exact same name on caller and listener before connect/listen:

```c
const char filter[] = "rwx-xor8-v1";
if (srt_setsockflag(socket, SRTO_PACKETFILTER, filter,
        sizeof(filter) - 1) == SRT_ERROR) {
    /* Report the configuration error. */
}
```

Accepted sockets inherit the selected profile. Both sides must explicitly
select it; the caller requires the matching filter CONFIG and HSRSP. An absent,
unknown or different identity fails setup. There is no fallback to unfiltered
traffic. This provider revision uses versioned names for complete fixed
configurations; arbitrary parameters and implicit FEC geometry merging are not
accepted. Matching names do not authenticate peers or attest matching binaries.

Payload capacity reserves 23 bytes for the nested DATA header, framing word
and worst-case padding. For a 1500-byte MSS this leaves at most 1433 application
bytes per source packet over IPv4, or 1413 over IPv6. Normal default payloads
remain unchanged when they fit. Sender `SRTO_PAYLOADSIZE` settings can differ;
receive admission uses the negotiated path capacity rather than the local
sender's payload preference.

## XOR eight-packet provider

`rwx-xor8-v1` emits one XOR repair for each complete block of eight original
member DATA sequences aligned to a multiple of eight. With equal source sizes,
repair payload overhead is 12.5%, plus the existing envelope and control framing.
A single missing source can be reconstructed from the repair and the other seven
sources. More losses require ordinary ARQ; receiving an ARQ source may then make
the remaining erasure recoverable. Initial and final partial blocks have no
repair. A source is protected only when its complete block was submitted.

The receiver caches 64 blocks per direction. Reordering, duplicate sources,
repair-first arrival, variable packet lengths, retransmission flags and sequence
rollover are supported within this bounded cache and the core receive window.
Older colliding blocks are discarded and rely on ARQ. The cache is a packet-count
bound, not a guaranteed time interval; at higher rates it covers less time.

Each instance declares 64 accumulators and bounded metadata, less than 128 KiB.
There are no event-path allocations, feedback messages or background threads.
The provider emits at most one symbol per callback and fits the existing source
payload budget, including at maximum MSS. Both endpoints must select the same
versioned name. This is a private experimental algorithm and wire format, not
Haivision FEC interoperability or production performance qualification.

The provider repair symbol is the XOR of encoded DATA bytes after the sequence
word, zero-extended to the longest source. Its first four bytes contain `0x58`,
version `1`, and the XOR of the eight encoded source lengths as a 16-bit network
byte-order value. The member envelope identifies the block's final sequence.
The missing sequence is reconstructed from that aligned block; the recovered
length, header, destination and payload are validated before candidate admission.
Retransmission flags are normalized before accumulation. XOR provides erasure
recovery, not authentication or corruption detection. Malformed or unusable
repair may leave a block to ARQ and cannot authorize application delivery.

## Compiled providers and lifecycle

Set `ROBOTWEAX_SRT_FILTER_PROVIDER=/absolute/path/provider.cpp` to replace the
bundled trusted source. It implements `compiled_filters()` from the private
`experimental/packet_filters/compiled_filter.hpp` header. The registry
copies at most 16 descriptors and unique names of 5–32 lowercase ASCII letters,
digits and hyphens, beginning with `rwx-`. Invalid descriptors or a conflicting
table disable custom selection. Provider code must remain loaded for every
instance's lifetime. This is a private experimental C++ interface, not an
installed or runtime-loaded plugin ABI.

The current member binding creates independent sender and receiver instances.
Each uses at most 128 KiB of declared provider state. Up to 4096 bytes use
inline storage; larger declarations reserve exactly their declared byte count
at setup, with checked alignment. State allocation failure rejects setup. Storage
outlives provider destruction; event callbacks do not resize it. Each instance
has up to four outputs and
5888 output bytes per callback. This binding admits member-transport scope,
no feedback, and no per-receiver state. Sender ticks and window changes are
delivered as lifecycle events; receiver callbacks run on source/repair input
and window changes. Receiver window-change callbacks must not emit candidates
in this first binding. The generic dispatcher retains broader scope, receiver
and feedback contracts for other coordinators.

Callbacks borrow inputs, write proposed output through core-owned storage,
and must not retain pointers, block, allocate on event paths, reenter socket
APIs or perform I/O. A successful construction has exactly one destruction;
failed construction must roll back partial state itself. Exceptions and invalid
callback outputs fail the affected connection. Malformed wire framing is dropped
before callbacks; providers must drop malformed opaque symbols by returning
success with no candidate. The example validates its nested DATA before doing
so. Native provider code has the application's privileges: these checks cannot
sandbox arbitrary memory access or enforce CPU-time bounds.

The executor serializes one instance, with no fixed thread affinity across
construction, events or destruction. Separate instances may execute the same
provider callbacks concurrently, including separate receiver instances for one
logical stream. Supplied storage belongs to that instance. Shared mutable
provider state requires synchronization consistent with nonblocking callbacks;
thread-local state cannot stand in for instance-owned state.

## Core budgets and receive policy

The core validates the complete output batch, copies it into a four-datagram
queue, and reserves repair credit before publishing any of it. Each submitted
original source funds at most its framed size in generated repair. Unused
credit is capped at 6000 bytes; polling cannot create traffic credit. Queue,
byte, identity or credit violations are terminal provider errors. When repair
is queued, sender ticks pause until it drains. This binding therefore bounds
both per-callback output and repair amplification across callbacks.

Batch validation and queue publication do not roll back state changed by a
callback. A coordinator must reserve capacity before calling the provider or
fail the affected instance if commit cannot complete. It must not replay the
same event merely because a queue was full. The current member binding treats
queue or repair-credit exhaustion as terminal.

Repair shares the source's pacing rate and send-attempt budget. Pending sends
use the existing bounded retry/lifecycle path. Repair does not create another
reliable DATA sequence or bypass peer flow control for original DATA. Its bytes
contribute to pacing and filter statistics.

Recovered candidates undergo complete-header, destination, encryption-selector,
payload and lifted sequence-window validation before any batch is exposed.
They then re-enter the core's reliability receive path. The core retains message
assembly, duplicate suppression, ACK/NAK policy, deadlines and TSBPD release.
An extension cannot mark candidate bytes authenticated or authorize delivery.
Later physical DATA gaps exclude already recovered or retired sequences from
NAKs, including when a late original is now a duplicate. Reconstructions do not
advance physical arrival, bandwidth or clock observations. Genuine unresolved
holes retain the ordinary reorder tolerance and periodic retransmission policy.
Loss classification remembers a prefix of the receive window. Repeated horizons
and repairs do not rescan that prefix; frontier movement exposes only newly
available slots. The first classification still does work proportional to the
newly exposed range. This adds one private size counter per receive buffer and
uses the existing reserved loss-range scratch storage.
`pktRcvFilterSupplyTotal` counts supplied candidates, including duplicates;
unique receive statistics count only candidates accepted as new DATA.

## Provisional wire binding

The name uses HSv5 type-7 CONFIG. Repair uses SRT USER_DEFINED control
(`0x7fff`), subtype `0x7f30`, with the member's 31-bit sequence in the
type-specific word and the ordinary destination socket ID. Its payload is:

| Field | Encoding |
| --- | --- |
| Version | One byte, `1` |
| Kind | One byte, `0` for repair |
| Symbol length | Unsigned 16-bit network byte order |
| Symbol | Provider-defined bytes; the example carries a complete clear DATA packet |
| Padding | Zero bytes to the next 32-bit boundary |

Unknown versions/kinds, nonzero padding, inconsistent lengths and out-of-window
sequences are dropped. This envelope has no authentication and is restricted
to the cleartext binding. Group or encrypted bindings need their own reviewed
identity, framing and protection contracts.

## Group-mode independence and validation

The generic filter boundary separates member transport from logical-stream
processing and separately identifies receiver ownership. It retains independent
member/logical sequences, path and receiver generations, shared source state,
feedback-free operation, unequal path budgets and protection domains. The
individual-socket adapter does not define the lifetime, routing or wire format
of a future aggregation or multicast coordinator.
The coordinator must preserve context and destination/receiver generations
with deferred output and check them again before submission. Routing repair
with no specified destination requires an eligible path that fits its budget;
it does not imply that every path fits or that every receiver must acknowledge.
Removing an instance's own member or receiver requires retiring that instance.
An inadmissible environment update leaves its previous snapshot intact, so the
coordinator must not continue using that snapshot as current membership.
Contract tests keep silent receivers active when the only feedback participant
leaves, preserving shared source state as optional feedback becomes unavailable.

Tests cover strict selection, cleartext bidirectional transfer, actual recovery
through the runtime, TSBPD/deduplication, sequence rollover, malformed framing
and symbols, invalid candidate batches, resource/credit exhaustion and rejected
profile combinations. A real UDP fault relay erases each of the eight XOR block
positions, blocks retransmitted DATA, and requires eight filter recoveries with
exact unique delivery despite repair replays, while the erased originals remain
absent. Physical DATA counts distinguish recovery from duplicate candidates. A complementary
case drops every repair and requires ordinary ARQ to recover the same erasures.
Separate contract tests exercise group/receiver churn.
The deterministic mutation target and coverage-guided `fuzz_extension_filter`
exercise framing, malformed symbols and rollover. Contract tests also exercise
concurrent receiver instances and synchronized thread handoffs. The checks cover their documented fixtures;
encrypted/group runtime integration remains unsupported. Each independently
supplied provider needs its own sustained-loss, large-BDP, network performance,
additional-platform and interoperability qualification.
