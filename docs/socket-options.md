# Socket options

The installed `<srt/srt.h>` interface is the public socket-configuration
surface. Use `srt_setsockflag()` and `srt_getsockflag()` with the declared
`SRT_SOCKOPT` value and value representation. Pre-connection, pre-bind,
post-connection, and read-only constraints are part of each option's contract.

## Identify the loaded crypto backend

Since 0.2.4, `SRTO_ROBOTWEAX_CRYPTO_BACKEND` (`0x01000002`) returns an
`int32_t`: `ROBOTWEAX_SRT_CRYPTO_BACKEND_OPENSSL` (1) or
`ROBOTWEAX_SRT_CRYPTO_BACKEND_BCRYPT` (2). This read-only query identifies the
loaded library, not the application's headers, selected cipher or remote peer.
It works on valid sockets and groups before connecting, including unencrypted
sockets, and never enables encryption or initializes a connection.

```c
int32_t backend = 0;
int length = sizeof(backend);
const char* name = "unknown";
if (srt_getsockflag(socket, SRTO_ROBOTWEAX_CRYPTO_BACKEND,
        &backend, &length) == 0) {
    if (backend == ROBOTWEAX_SRT_CRYPTO_BACKEND_OPENSSL) name = "OpenSSL";
    else if (backend == ROBOTWEAX_SRT_CRYPTO_BACKEND_BCRYPT) name = "BCrypt";
}
```

Create a valid socket after `srt_startup()` if the application has none yet,
then close it when finished. Null pointers and buffers smaller than four bytes
are rejected without writing the result; success sets the length to four.
Setting this option is unsupported. Older Robotweax and other SRT libraries may
reject the query: report unknown, not OpenSSL, on failure or an unknown future
value. When compiling against older headers, guard the example with
`#ifdef ROBOTWEAX_SRT_CRYPTO_BACKEND_OPENSSL`. No exported symbol, structure
layout, upstream option value or `SRTO_E_SIZE` changes.

## Identify the local implementation

`<srt/srt.h>` exposes `ROBOTWEAX_SRT_VERSION_MAJOR`, `_MINOR`, `_PATCH`,
`_STRING` and `_VALUE` (each with the `ROBOTWEAX_SRT_VERSION` prefix).
These describe the headers' product release. They do not replace `SRT_VERSION_*`,
which describe SRT compatibility. The packed numeric format is
`major * 0x10000 + minor * 0x100 + patch`.

`SRTO_ROBOTWEAX_VERSION` (`0x01000001`) is a read-only extension returning an
`int32_t` containing the loaded library's product version. Query it using
`srt_getsockflag()` with a valid socket or group, even before connecting. An
`int` length must initially be at least `sizeof(int32_t)`; success sets it to
that size. Null pointers or short buffers are invalid. The option is local:
it is never negotiated or transmitted and says nothing about the peer.

```c
#if defined(ROBOTWEAX_SRT_VERSION_VALUE)
int32_t product_version = 0;
int length = sizeof(product_version);
if (srt_getsockflag(socket, SRTO_ROBOTWEAX_VERSION,
        &product_version, &length) == 0) {
    /* product_version identifies the loaded implementation, not the headers. */
}
#endif
```

Failure means identification was unsuccessful, not proof of Haivision: older
Robotweax releases lack this option, and invalid handles also fail. Handle the
error normally. `srt_getversion()`, `SRTO_VERSION`, `SRTO_PEERVERSION` and all
existing option numbers retain their compatibility semantics. This extension
adds no exported function or structure field. A high option number reduces
collision risk but is not an upstream namespace reservation.

```c
#include <srt/srt.h>

SRTSOCKET socket = srt_create_socket();
int64_t input_bandwidth = 2000000;
int32_t overhead_percent = 20;
int32_t receiver_latency_ms = 300;

if (srt_setsockflag(socket, SRTO_INPUTBW,
        &input_bandwidth, sizeof(input_bandwidth)) == SRT_ERROR
    || srt_setsockflag(socket, SRTO_OHEADBW,
        &overhead_percent, sizeof(overhead_percent)) == SRT_ERROR
    || srt_setsockflag(socket, SRTO_RCVLATENCY,
        &receiver_latency_ms, sizeof(receiver_latency_ms)) == SRT_ERROR) {
    /* Inspect srt_getlasterror() or srt_getlasterror_str(). */
}
```

Selected post-connection options—including input/minimum/maximum bandwidth,
overhead, drift tracing, sender drop delay, and maximum reorder tolerance—can
be reapplied without recreating the socket. See the option inventory below and
the exact machine-readable scope in
[`compat/srt-1.5.7-api.json`](../compat/srt-1.5.7-api.json).

## Native packet/options C boundary

```c
#include <robotweax_srt.h>

robotweax_srt_options* options = NULL;
if (robotweax_srt_options_create(&options) != ROBOTWEAX_SRT_OK)
    return 1;

robotweax_srt_options_set(options, ROBOTWEAX_SRT_OPT_INPUT_BANDWIDTH, 2000000);
robotweax_srt_options_set(options, ROBOTWEAX_SRT_OPT_OVERHEAD_PERCENT, 20);
robotweax_srt_options_set(
    options, ROBOTWEAX_SRT_OPT_MAXIMUM_REORDER_TOLERANCE, 10);
robotweax_srt_options_set(options, ROBOTWEAX_SRT_OPT_TRANSMISSION_TYPE, 1);

int64_t bandwidth = 0;
robotweax_srt_options_get(options, ROBOTWEAX_SRT_OPT_INPUT_BANDWIDTH, &bandwidth);
robotweax_srt_options_destroy(options);
```

The `<robotweax_srt.h>` option type is opaque. Its layout is not part of the
ABI, and destroying a null handle is safe. Invalid option identifiers and
invalid values have distinct error codes. This small native interface validates
Robotweax option values independently of an SRT socket; applications normally
configure live transport through `<srt/srt.h>`.

## Available options

- Input, minimum-input, and maximum bandwidth in bytes per second
- Bandwidth overhead percentage
- Receiver and peer latency
- Sender drop delay
- TSBPD and too-late packet drop
- Periodic NAK and retransmit flag support
- Maximum adaptive reorder tolerance in packets
- Send and receive buffer sizes in packets
- Native UDP send and receive buffer sizes in bytes
- Flow-control window in packets
- Maximum data payload size
- Passphrase (empty, or 10–80 bytes)
- AES key length (default/128, 192, or 256 bits)
- Key refresh rate and key pre-announcement period
- Enforced-encryption policy and live key-material states
- Rendezvous connection mode
- Message/buffer API selection and the live/file transmission type
- Independent `"live"`/`"file"` congestion-controller selection
- Compatible live/file linger configuration
- Packet-filter configuration through a bounded pre-connection string
- Caller Stream ID through a bounded pre-connection string
- Compatible pre-connection sender-role hint
- Read-only local initial data sequence number
- Read-only peer SRT software version and incoming listener group type
- Minimum accepted peer SRT version and receiver drift-tracer control
- Native IP time-to-live/hop-limit and IPv4/IPv6 traffic class
- Linux network-interface binding with an explicit unsupported-platform result

## AES-GCM extension option

`SRTO_CRYPTOMODE` is deliberately absent from the default SRT v1.5.7 public
profile. It is available only when Robotweax is configured with the historical
build flag
`-DENABLE_AEAD_API_PREVIEW=ON`, which also exports that compile definition to
consumers of the CMake target. The pre-connection `int32_t` values are `0`
for AUTO, `1` for AES-CTR, and `2` for AES-GCM. GCM accepts exactly two coherent
transport bundles: Live/Message with LiveCC and TSBPD enabled, or File/Stream
with FileCC and TSBPD disabled. Incompatible partial mutations fail with
`SRT_EINVPARAM`; `SRTO_TRANSTYPE` changes the complete bundle atomically in
either option-setting order. After connection, the getter returns the effective
concrete mode.

The released, default-off extension accepts explicit GCM with the implemented
IPv4/IPv6 Live Message and File/Stream Caller/Listener and Rendezvous option
bundles. It also
accepts the built-in `fec` filter before or after GCM for Live/Message and
reserves both the four-byte filter header and 16-byte GCM tag. FEC with the
File/Stream bundle remains invalid and fails with `SRT_EINVPARAM` in either
option-setting order. Robotweax additionally accepts `SRTO_CRYPTOMODE` on a
Broadcast or Backup group before its first connection and copies the requested
mode into every initial, replacement, or late-joining member. The group getter
reports the configured member default; connected-member getters report their
effective negotiated modes.

IPv6 reserves
64 bytes of network/SRT overhead and the 16-byte GCM tag, yielding connected
plaintext ceilings of 1200 bytes at MSS 1280 and 1420 bytes at MSS 1500.
File/Stream at MSS 1500 uses 1440 bytes over IPv4 and 1420 over IPv6. At IPv4
MSS 1500, Live/Message FEC plus GCM has a 1436-byte plaintext ceiling and a
1452-byte protected FEC payload ceiling. AES-192/256 reference transfer,
cross-implementation File/Stream, cross-implementation Live Caller/Listener
rotation, and mixed-reference encrypted groups remain outside the released
interoperability claims. See [Compatibility status](compatibility.md).

The compatible `<srt/srt.h>` boundary exposes these as
`SRTO_PASSPHRASE`, `SRTO_PBKEYLEN`, `SRTO_KMREFRESHRATE`,
`SRTO_KMPREANNOUNCE`, `SRTO_ENFORCEDENCRYPTION`, and the three read-only
KM-state options. `SRTO_SNDBUF` and `SRTO_RCVBUF` are pre-bind `int32_t` byte
counts backed by the actual SRT packet rings. Their 8,192-slot defaults report
12,058,624 bytes. Set values are converted to 1,472-byte slots, with a minimum
of 32 slots; the receive ring is additionally capped by `SRTO_FC`. Successful
application reads schedule a full ACK immediately so a peer that stopped at a
zero advertised receive window can resume.
`SRTO_FC` remains the socket's local configured receive-window value after
HSv5 Caller/Listener or Rendezvous establishment. The runtime tracks the peer's
handshake-advertised window separately for outbound DATA and replaces only that
private sender limit when a current Small or Full ACK advertises a newer capacity.

For every compatible Boolean `SRTO_*` setter, Robotweax accepts either the
one-byte C/C++ `bool` representation or the documented four-byte `int32_t`
representation containing exactly 0 or 1. Getters continue to return the
declared one-byte Boolean value. Other sizes and integer values are rejected
with `SRT_EINVPARAM`; this keeps the permissive source ABI explicit without
silently treating arbitrary integers as true.

`SRTO_PEERVERSION` is a read-only `int32_t` observation. Before a successful
HSv5 exchange it is zero, including on the provisional socket while
`srt_listen_callback` is executing. After admission and connection completion,
each connected caller or accepted socket retains the peer's `0x00XXYYZZ` SRT
software version. On a connection group the getter derives the value from its
first member and returns zero while the group is empty.

`SRTO_GROUPTYPE` is a different, deliberately callback-scoped observation. On
the provisional socket passed to `srt_listen_callback`, it reports the incoming
`SRT_GTYPE_BROADCAST` or `SRT_GTYPE_BACKUP` declaration. Ordinary connections
and calls outside the listener callback deterministically return
`SRT_GTYPE_UNDEFINED`; group handles reject the option because the documented
v1.5.7 API defines it as socket-only. Both options are read-only. This keeps
incoming admission metadata separate from the accepted socket's eventual
group membership and avoids exposing stale callback state.

`SRTO_MINVERSION` is a pre-connection `int32_t` encoded as `0x00XXYYZZ` and
defaults to SRT 1.0.0 (`0x00010000`). An explicitly advertised lower peer
version is rejected with `SRT_REJ_VERSION` in caller/listener and Rendezvous
handshakes. Independently, an integrated HSv5 HSREQ/HSRSP cannot coherently
claim a software version older than SRT 1.3.0; that malformed combination is
rejected with `SRT_REJ_ROGUE`, matching the selected compatibility target. An
omitted optional HSv5 response extension is not fabricated as version zero;
it therefore remains compatible with peers that legitimately omit the
response echo. Broadcast and Backup groups derive this policy for caller
members and listener-created mirrors. Robotweax SRT v0.1.0 also exposed the
real peer library version after its historical HSv4 setup; that behavior is not
part of the 0.2 production baseline. Version 0.2 preserves the
public 1.0.0 option default and the separate 1.3.0 HSv5 coherence floor; see
the [0.1-to-0.2 migration guide](migration-0.2.md).

`SRTO_DRIFTTRACER` is a post-connection Boolean, defaults to `true`, and gates
receiver-side TSBPD drift samples from forward unique DATA and KEEPALIVE
timestamps dynamically. Disabling it freezes drift correction without
discarding the negotiated TSBPD clock mapping; re-enabling it resumes
sampling. This is payload-agnostic—the transport never parses MPEG-TS PCR or
assumes 188/1316-byte framing.

`SRTO_MININPUTBW` is a nonnegative post-connection `int64_t` floor for the
fixed-memory application-input estimator. It takes effect only in relative
LiveCC mode when both `SRTO_MAXBW=0` and `SRTO_INPUTBW=0`; configured input
bandwidth, an absolute positive maximum, or the `SRTO_MAXBW=-1` one-gigabit
live ceiling retain their documented precedence. Groups derive and propagate
the value to current and future members.

`srt_connection_time` returns the socket-open instant in microseconds in the
same process-local monotonic epoch used by `srt_time_now` and source-time
metadata. The value is stable for the socket lifetime. Invalid, group, and
closed handles return `-1` with `SRT_EINVSOCK`; it is not a wall-clock value.

`SRTO_MSS` is a pre-bind `int32_t` and defaults to 1,500 bytes. Values from
76 through 1,500 are accepted. The value includes the IP, UDP, and SRT
headers, is advertised in the handshake, and both peers use the lower
advertised value. The transport subtracts 44 bytes for IPv4 or 64 bytes for
IPv6 when deriving its data-payload ceiling. Changing MSS also realigns the
compatible SRT buffer byte values to `MSS - 28`, matching the reference API's
packet-slot convention, and constrains `SRTO_PAYLOADSIZE`. MSS values above
1,500 remain unsupported until the fixed receive-datagram storage is extended
for jumbo packets. A remote UDP datagram that exceeds that storage is detected
and discarded as one indivisible message; its truncated prefix is never passed
to SRT packet decoding and does not break other sockets sharing the channel.

`SRTO_UDP_SNDBUF` and `SRTO_UDP_RCVBUF` are also pre-bind `int32_t` byte
counts. Positive values below the configured MSS are clamped to MSS.
Their effective values are applied to `SO_SNDBUF` and `SO_RCVBUF` before
explicit listener binding or implicit caller binding; accepted sockets inherit
the listener values. A getter returns the portable effective SRT value rather
than an operating-system-specific doubled or clamped kernel value.
On macOS/BSD, an `ENOBUFS` rejection of a UDP buffer request uses a
best-effort fallback, for both created and acquired UDP sockets: an existing
buffer of at least 64,000 bytes is retained; otherwise 64,000 bytes is attempted
when the request was at least that large. Other errors and failed fallbacks
remain errors. This applies to explicit settings as well as defaults, matching
Haivision's best-effort macOS/BSD behavior without shrinking an already useful
buffer. Explicit bind/acquire and caller setup report requested and actual
buffer bytes through the socket-management warning logger after releasing
internal locks. The SRT option getter still reports the configured value, not
the kernel allocation. Size buffers and validate packet loss under realistic
traffic; a successful bind alone does not qualify throughput.
`SRTO_REUSEADDR` is a pre-bind boolean and defaults to `true`. Independent
IPv4 or IPv6 SRT sockets that bind the same exact address and port with
matching native UDP buffer settings share one UDP channel and its
destination-socket-ID
dispatcher. Setting reuse to `false`, using incompatible UDP settings, or
requesting a wildcard binding that conflicts with an existing in-process
binding fails with `SRT_EBINDCONFLICT`. A shared channel permits multiple
connections and survives until its last SRT owner closes; only one listener may
own the address at a time.
`SRTO_IPV6ONLY` is a pre-bind `int32_t` with the documented range `-1..1`.
The default `-1` retains the operating-system default for implicit binding and
for specific IPv6 addresses. An explicit bind to the IPv6 wildcard `::`
requires 0 for an IPv4/IPv6 dual-stack socket or 1 for an IPv6-only socket.
The binding registry includes that effective mode in overlap, sharing, and
`SRT_EBINDCONFLICT` decisions. Acquired IPv6 UDP sockets report their actual
native `IPV6_V6ONLY` value, and bound sockets report the effective native mode
instead of the original `-1` preset.
`SRTO_IPTTL` and `SRTO_IPTOS` are pre-bind `int32_t` options. TTL accepts
1–255 and defaults to 64; TOS accepts 0–255 and defaults to `0xB8`. Robotweax
maps them to `IP_TTL`/`IP_TOS` for IPv4 and, where available,
`IPV6_UNICAST_HOPS`/`IPV6_TCLASS` for IPv6. Before binding, getters return
the configured preset; afterwards they inspect the actual native UDP socket.
Explicit and implicit binds, acquired sockets, accepted sockets, and
connection-group members use the same configuration path. Exact shared binds
may reuse a UDP channel only when these values also match.

Winsock does not expose `IPV6_TCLASS` as a settable IPv6 socket option.
Robotweax therefore leaves the unmodified `0xB8` preset unapplied so ordinary
Windows IPv6 binding and acquisition remain usable. An explicitly configured
IPv6 `SRTO_IPTOS` value fails when the IPv6 family becomes known with
`SRT_EINVOP`; it is never silently reported as applied. A bound getter returns
the untouched preset in the default case. Windows IPv4 TOS and IPv4/IPv6 TTL
remain native. A future Windows QoS-flow backend can close this platform gap
without changing the public option ABI.
`SRTO_BINDTODEVICE` is deliberately platform-specific. On Linux it maps to
`SO_BINDTODEVICE`, accepts an empty name or at most 15 interface-name bytes,
and preserves native permission/device errors at bind time. Its getter follows
the v1.5.7 fixed 16-byte buffer contract and inspects the native socket after
binding. On macOS and Windows, and on every group handle, get/set fail with
`SRT_EINVOP`; Robotweax does not claim a portable effect that the operating
system cannot provide. Per-member group endpoint configuration may still set
the option for Linux member sockets.

The v1.5.7-compatible `SRT_SOCKOPT_CONFIG` matrix is preserved. Robotweax also
accepts `SRTO_PASSPHRASE`, `SRTO_PBKEYLEN`, and
`SRTO_ENFORCEDENCRYPTION` in that opaque endpoint object as an ABI-neutral
extension. Applications may instead set those options, together with
`SRTO_KMREFRESHRATE`, `SRTO_KMPREANNOUNCE`, and `SRTO_PEERIDLETIMEO`, on an
unopened group handle. Every future member receives one bounded copy before
connect; later changes fail with `SRT_ECONNSOCK`, and the passphrase remains
write-only. Crypto state, timestamps, ACKs, and rotation remain independent per
member.
`SRTO_RENDEZVOUS` is a pre-connection boolean: rendezvous
connect requires an explicitly bound socket, and listen/accept is invalid in
that mode. `SRTO_TRANSTYPE=SRTT_FILE` is a pre-bind bundle that selects
FileCC, maximum payload, buffer-mode extraction, and disables the live-only
TSBPD/TLPKTDROP/periodic-NAK behavior. `SRTO_MESSAGEAPI` may select message
extraction before connection; the STREAM handshake flag is derived from that
same value. `SRTO_LINGER` accepts `struct linger`: live mode defaults to
`{0, 0}`, file mode to `{1, 180}`, and synchronous close waits for send-buffer
drain up to that deadline. With `SRTO_SNDSYN=false`, close returns in
`SRTS_CLOSING` and a shared lifecycle worker completes the close after drain or
deadline. `SRTO_LOSSMAXTTL` is a nonnegative, runtime-mutable `int32_t`.
Its default of zero reports new gaps immediately. A positive value enables
packet-count-based Fresh-Loss aging and bounds the adaptive receive reorder
tolerance; it does not initialize the current tolerance to the bound.
Increasing the bound preserves the current estimator, while reducing it clamps
the current value. Accepted sockets inherit the listener's value.
`SRTO_PACKETFILTER` accepts up to 512 bytes using
`fec,cols:N[,rows:N][,layout:even|staircase][,arq:never|onreq|always]`.
Enabling it reserves the four-byte FEC control header from the maximum live
payload and participates in HSv5 negotiation. Row, Column, and recursively
composed Matrix paths are supported for Caller/Listener and Rendezvous in the
documented Live/Message combinations. File/Stream plus FEC, arbitrary filter
plugins, and unbounded geometry are not supported. See
[Packet filters and FEC](packet-filter.md).

`SRTO_CONGESTION` is a pre-connection, write-only string option. It accepts
exactly `"live"` or `"file"` without the terminating NUL byte. Directly
setting it changes only the congestion controller; `SRTO_TRANSTYPE` remains
the recommended way to select the complete live/file option bundle. FileCC is
advertised with the optional HSv5 type-6 CONFIG extension. An absent request
means LiveCC, and unequal peers reject the connection with
`SRT_REJ_CONGESTION`. A successful responder is not required to echo an
accepted CONFIG extension; if it supplies one, the value must match. FileCC
supports NAK and flight-tail RTO recovery, dynamic bandwidth limits, and
Caller/Listener or Rendezvous within the combinations documented in
[File/Stream mode](file-mode.md). Large-BDP and long-duration deployment
behavior must be validated for the intended network.

`SRTO_SENDER` is a pre-connection boolean and defaults to `false`. It remains a
compatible directional hint for role-sensitive packet-filter peers and is not
a handshake-generation selector. It supports round-trip get/set behavior until
connection establishment; later sets fail with `SRT_ECONNSOCK`. In v0.1.0 it
also selected the independent post-connect HSv4 data role; positive HSv4 setup
is absent from 0.2, but the public option remains supported for its modern
directional uses.

`SRTO_GROUPCONNECT` is a pre-connection boolean on listener sockets and
defaults to `false`. Robotweax accepts both the one-byte C++ `bool` form and
the compatible four-byte 0/1 integer form; other values are rejected. When
enabled before `srt_listen`,
compatible Broadcast and Backup membership requests create listener-scoped
mirror groups. Calling `srt_accept_bond` over multiple listeners explicitly
places them in one bond domain, allowing members received through different
listeners to join the same mirror. The first connected member makes the group
available through `srt_accept` or `srt_accept_bond`; subsequent members join
the same mirror without an additional accept result. A listener callback may
change the option on its provisional connection before the conclusion is
interpreted. Unsupported types, inconsistent group metadata, and allocation
or admission failures are rejected transactionally with the group rejection
reason.

Broadcast group handles own `SRTO_SNDSYN`, `SRTO_RCVSYN`, `SRTO_SNDTIMEO`,
and `SRTO_RCVTIMEO`. These values control the logical group operation; they do
not mutate the independently scheduled member transports. A mirror group
created by a listener inherits the listener's four logical I/O values before
its first member is published. The synchronization booleans accept both the
one-byte C `bool` representation and the compatible four-byte integer
representation. Broadcast and Main/Backup payload I/O are implemented.
Group-wide security and peer-idle defaults apply to subsequently created
members. See [Connection Groups](connection-groups.md) for inheritance and
failover boundaries.

`SRTO_STREAMID` is a pre-connection string with an empty default and a maximum
length of 512 bytes. The Caller sends a non-empty value in the HSv5 type-5
extension. The Listener reads it from the connected socket returned by
`srt_accept`; it is not inherited from the listening socket. A registered
`srt_listen_callback` receives the same value as a NUL-terminated argument
before final connection admission, and the provisional socket exposes it
through `srt_getsockflag`. The callback may change connection-stage options for
that accepted socket without changing the listening socket. Getters require
space for the value plus a terminating NUL and report the exact value length.
Although the protocol initiator can also send this extension in Rendezvous,
the resulting winner depends on the cookie-selected initiator role, so the
option is intentionally documented as unsuitable for deterministic Rendezvous
routing.

`SRTO_ISN` is a read-only `int32_t` containing the socket's 31-bit initial
data sequence number. It is stable throughout the socket lifetime. In a
classic caller/listener connection, the accepted socket exposes the negotiated
caller ISN; in rendezvous mode, each socket exposes its local directional ISN.
Attempts to set the option fail with `SRT_EINVOP`.

`SRTO_EVENT`, `SRTO_SNDDATA`, and `SRTO_RCVDATA` are read-only `int32_t`
options for individual sockets. `SRTO_EVENT` returns the currently active
combination of `SRT_EPOLL_IN`, `SRT_EPOLL_OUT`, and `SRT_EPOLL_ERR` from the
same readiness snapshot used by the public epoll API, including TSBPD delivery
deadlines and terminal states. `SRTO_SNDDATA` reports the number of packets
still retained in the unacknowledged send ring. `SRTO_RCVDATA` reports occupied
receive-ring packets, including data buffered ahead of its TSBPD playout time;
it therefore describes buffer occupancy rather than immediate application
readability. Before a runtime exists, both packet counts are zero. Setting any
of the three options, or retrieving them from a connection-group handle, fails
with `SRT_EINVOP`.
