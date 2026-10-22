# Runtime construction during handshake handoff

An established peer may send its first DATA/FEC flight immediately behind the
handshake. Before the receiver installs its connection runtime, the shared UDP
channel routes those datagrams into a bounded 256-entry setup inbox.

Runtime construction allocates the reliability and FEC buffers. Constructing it
outside the channel routing lock lets the channel keep filling that smaller
inbox during allocation. A 250-source-packet matrix-FEC flight also contains 75
parity packets and key-exchange controls, so it can exceed the inbox capacity.
The resulting local drops can strand the transfer or omit parity statistics.

A private Debug reproduction against the pinned Haivision 1.5.7 public-API peer
observed 73 inbox overflows and received 259,252 of 329,000 payload bytes. The
reference sender and Robotweax receiver failed after approximately 30 seconds.
This reproduces a concrete local defect. The original hosted failure had a
different completion timing; its exact cause cannot be established from the
existing trace alone. A successful hosted rerun does not resolve that uncertainty.

## Correction

The channel now constructs the runtime through a factory while holding its
routing lock. Concurrent ingress waits at route dispatch, leaving the remaining
flight in the native UDP receive queue until construction and FIFO setup-inbox
replay finish. The temporary inbox remains bounded at 256 entries; the receive
buffer settings, transport deadlines, FEC criteria, and public C ABI are unchanged.

The factory receives a configuration snapshot captured under the socket lock.
That lock is released before acquiring the channel routing lock. The factory
only constructs a runtime and must not call back into the channel. Invalid setup
routes do not invoke it. A null result or allocation exception leaves the setup
route and queued input intact; the C API retains its existing failure mapping.

Runtime allocation now holds the shared channel's routing lock. Other routes on
that channel can therefore wait for construction, as they already wait during
setup-inbox replay. This change adds no worker or unbounded application queue;
it does not guarantee delivery when native UDP buffering or host scheduling is
insufficient.

## Validation

The deterministic promotion test checks invalid routes, null factories, allocation
failure, successful retry, and FIFO delivery of queued DATA. These tests do not
model host scheduling or claim to reproduce a timing race deterministically.

The privately instrumented corrected Debug build completed 20 attempts of each
of the encrypted matrix-FEC rendezvous single-drop and burst-drop profiles,
covering both rendezvous roles, without an inbox overflow. Every attempt retained
the existing byte-integrity, reconstruction, parity-count, and key-transition
assertions. Instrumentation is not included in the production change.

Use the normal full native and Python checks, plus the FEC interoperability runner,
to validate a candidate. The focused diagnostic selector preserves every failure:

```sh
./tools/python interop/run_fec_interop.py \
  --robotweax-peer build/robotweax_srt_interop_peer \
  --reference-peer reference-api-peer \
  --rendezvous-scenario aes256-matrix-fec-rendezvous-haivision-to-robotweax \
  --repeat 20
```

Repeat for the `-burst` profile. Keep baseline failures and corrected results
separately; later passes must not replace failed observations.
