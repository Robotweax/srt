# Experimental compiled congestion controllers

Development feature, default OFF. Enable with
`ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_CONTROLLERS=ON`. It adds compiled controller
names to the existing pre-connection, write-only `SRTO_CONGESTION` option.
The default build continues to accept only its documented built-in names.

The initial experimental runtime profile is Live Message Caller/Listener with
TSBPD, cleartext or AES-CTR. Groups, Rendezvous, packet filters and AES-GCM are
rejected for this profile. These restrictions apply to this controller runtime;
packet-filter extensions remain a separate feature. This controller interface
does not define future aggregation or multicast routing and receiver ownership.

## Build and select

```sh
cmake -S . -B build-controllers -DCMAKE_BUILD_TYPE=Debug \
  -DROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_CONTROLLERS=ON
cmake --build build-controllers --parallel
ctest --test-dir build-controllers --output-on-failure
```

The bundled provider exposes `rwx-fixed-v1`: a demonstration controller that
requests 1,000,000 bytes/s and a 32-packet window. The core applies configured
bandwidth, flow-control and peer receive-window limits. It is not a qualified
adaptive algorithm or a throughput guarantee.

Both caller and listener must set the name before connect/listen:

```c
const char controller[] = "rwx-fixed-v1";
if (srt_setsockflag(socket, SRTO_CONGESTION, controller,
        sizeof(controller) - 1) == SRT_ERROR) {
    /* Report the configuration error. */
}
```

Accepted sockets inherit the listener's selection. Controller names travel in
the existing HSv5 congestion CONFIG record; local registry indices never travel
on the wire. Unlike the ordinary built-in CONFIG contract, a custom caller
requires an explicit matching response and HSRSP. Missing confirmation,
mismatched names and unknown names fail setup. There is no downgrade to LiveCC.
Matching names select an algorithm; they do not authenticate a peer or attest
that its binary implementation matches the local one.

## Supply a compiled provider

Set `ROBOTWEAX_SRT_CONTROLLER_PROVIDER=/absolute/path/provider.cpp` when building
the library. That trusted C++ source implements
`robotweax::srt::extensions::compiled_controllers()` from the private source-tree
header `experimental/transport_extensions/compiled_congestion.hpp`, returning a
descriptor table. The bundled `fixed_controller.cpp` demonstrates the contract.
The provider interface is experimental and is not an installed plugin ABI.

The registry copies at most 16 descriptors with names of 1–32 lowercase ASCII
letters, digits or hyphens. Duplicate names, built-in name collisions, unknown
interface revisions, missing callbacks, excessive state sizes and unsupported
alignments invalidate the registry. Instance state is limited to 1024 bytes,
aligned to `std::max_align_t`. Descriptor callbacks and names are copied at setup;
callback code must remain loaded for the instance lifetime. Use a distinct
versioned name whenever compatibility changes.

The core invokes callbacks under the owning connection's serialization.
Callbacks must not block, allocate on event paths, call socket APIs, reenter
the transport or keep borrowed references. Construction must roll back partial
state on failure, and destruction must not throw. These rules govern trusted
native code; the wrapper does not sandbox arbitrary memory access or blocking.
No peer can load code or select a provider that was not compiled locally.

Serialization applies to one instance. Different connections can execute the
same provider callbacks concurrently, and an instance can move between caller
and scheduler threads after synchronization. Construction, events and destruction
have no fixed thread affinity. Keep mutable algorithm state in the supplied
instance storage; shared mutable provider state needs synchronization that
preserves the nonblocking callback contract. Do not use a thread-local variable
as a substitute for connection-owned state.

## Runtime behavior and limits

Callbacks receive core-clock ticks, validated ACK progress, loss and timeout
events, RTT and current core limits. They return a positive integer byte rate
and packet window. Core caps apply before pacing uses those decisions. Positive
`SRTO_MAXBW` changes update the cap at runtime; nonpositive values impose no
additional rate cap on the provider. The fixed example does not use LiveCC's
`INPUTBW`, `MININPUTBW` or `OHEADBW` estimation behavior.

Sequence validation, ACK cadence, periodic-NAK policy, retransmission selection,
TSBPD, peer flow control, crypto and shutdown stay in the core. Controller
errors, exceptions, invalid decisions or backwards core time are terminal:
setup fails, or an established connection becomes broken. Invalid output is
never used and an established connection never silently changes algorithms.

Deterministic tests exercise capped pacing, controller-window blocking,
validated feedback, dynamic cap changes and failure handling. Loopback tests
cover bidirectional cleartext/CTR delivery, accepted-socket inheritance,
one-sided selection and missing response confirmation. The
[testing guide](testing.md#sanitizers-and-fuzzing) describes controller mutation
and coverage-guided fuzz targets and sanitizer runs. Contract tests also execute
independent instances concurrently and
hand them between threads with explicit synchronization. These checks cover
the tested fixtures; they do not qualify an independently supplied adaptive
algorithm. Each provider still needs its own sustained-loss, large-BDP,
cross-platform, third-party-peer and network performance qualification.
