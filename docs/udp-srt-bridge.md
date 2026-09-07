# Live MPEG-TS UDP ↔ SRT bridge

This demo is an unreleased addition; it is not included in the `v0.2.3` tag.

`robotweax_srt_udp_bridge` demonstrates a continuous live bridge using only
the public `<srt/srt.h>` API. The same executable has two application modes:

```text
MPEG-TS source ──UDP──> bridge send ──SRT──> bridge receive ──UDP──> player
```

There is no transcoding, remuxing, RTP encapsulation, or private control frame.
Each input UDP datagram becomes one SRT Message and one output UDP datagram.
Payload bytes are unchanged. SRT's live receiver delivery timing is used;
the bridge does not parse PCR, restamp timestamps, or promise hard-real-time
UDP output. This is an example application, not a supervised production relay.

## Build

Prerequisites: Git, CMake 3.20+, a C++20 compiler, and OpenSSL 3 development
headers/libraries. See [Building](building.md). From the repository root:

```sh
cmake -S . -B build-bridge \
  -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_BUILD_EXAMPLES=ON \
  -DROBOTWEAX_SRT_BUILD_TESTS=OFF \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
cmake --build build-bridge --target robotweax_srt_udp_bridge --parallel 2
```

For Visual Studio, add `--config Release` to the build command. The executable
is then typically `build-bridge/Release/robotweax_srt_udp_bridge.exe`.
The examples are opt-in and are not installed as production utilities.

## Quick local test

Use three or four terminals. Start the UDP destination/player first, then the
SRT listener, then the SRT caller, and finally the UDP source. The bridge
prints `CONNECTED` when ready to forward. Do not start the source before both
bridges are connected: this demo has no pre-connection recording buffer.

### 1. UDP player (optional external software)

For example, using an independently installed FFmpeg/ffplay:

```sh
ffplay -f mpegts 'udp://127.0.0.1:5001'
```

VLC can instead open the network URL `udp://@127.0.0.1:5001`.
These players are not bundled with Robotweax SRT.

### 2. SRT receiver → UDP output

```sh
./build-bridge/robotweax_srt_udp_bridge receive \
  --srt-mode listener --srt-host 127.0.0.1 --srt-port 9000 \
  --udp-host 127.0.0.1 --udp-port 5001 --latency-ms 120
```

Wait for `READY SRT listener`.

### 3. UDP input → SRT sender

```sh
./build-bridge/robotweax_srt_udp_bridge send \
  --srt-mode caller --srt-host 127.0.0.1 --srt-port 9000 \
  --udp-host 127.0.0.1 --udp-port 5000 --latency-ms 120
```

### 4. Feed the live source

Configure your existing raw MPEG-TS-over-UDP source to send to
`127.0.0.1:5000`, preferably with 1316-byte datagrams. Alternatively, replay a
local file in real time with FFmpeg:

```sh
ffmpeg -re -i sample.ts -map 0 -c copy -f mpegts \
  'udp://127.0.0.1:5000?pkt_size=1316'
```

The bridge prints `STATUS` once per second, including during input silence.
Forwarding counts are local counts, not end-to-end acknowledgements or proof
of lossless delivery. See the status and reconnect sections below.
Stop the source and use Ctrl+C to stop each bridge. There is no EOF message
inside the MPEG-TS payload and no guarantee to drain pending data on Ctrl+C.

## Two machines

On the receiving machine, use `--srt-host 0.0.0.0` to listen on all IPv4
interfaces. On the sending machine, set `--srt-host` to the receiver's numeric
IPv4 address. Allow the chosen SRT UDP port through the receiver's firewall;
configure UDP port forwarding if the receiver is behind NAT.

`--udp-host` means **bind address** in `send` mode and **output destination**
in `receive` mode. Use a local interface address (or `0.0.0.0`) to receive
UDP from another device. Use the player's address for remote UDP output.
Loopback addresses are the safer default when testing on one machine.

The SRT connection initiator is independent of media direction. Both `send`
and `receive` support Caller, Listener, and Rendezvous. A sender can also use
`--srt-mode listener` while the receiver uses `--srt-mode caller`.
Only one peer is served per invocation.

## Rendezvous in either media direction

Select `--srt-mode rendezvous` on **both** applications. Each binds its own
`--srt-bind-host` / `--srt-local-port` and connects to the other application's
`--srt-host` / `--srt-port`. Start both within the connection timeout window.
For an interactive test, the commands below allow 30 seconds.

On one machine, use different local SRT ports, cross-connected as follows.
The UDP source/player ports remain 5000 and 5001:

```sh
./build-bridge/robotweax_srt_udp_bridge send \
  --srt-mode rendezvous \
  --srt-bind-host 127.0.0.1 --srt-local-port 9001 \
  --srt-host 127.0.0.1 --srt-port 9002 \
  --udp-host 127.0.0.1 --udp-port 5000 \
  --connect-timeout-ms 30000
```

```sh
./build-bridge/robotweax_srt_udp_bridge receive \
  --srt-mode rendezvous \
  --srt-bind-host 127.0.0.1 --srt-local-port 9002 \
  --srt-host 127.0.0.1 --srt-port 9001 \
  --udp-host 127.0.0.1 --udp-port 5001 \
  --connect-timeout-ms 30000
```

Wait for `CONNECTED` on both sides before starting the UDP source. The earlier
`CONNECTING SRT rendezvous` line only announces an attempt, not an established
connection. Optional AES-CTR works in Rendezvous as described below.

On separate machines, use an appropriate local bind address (default
`0.0.0.0`) and the peer's reachable IPv4 address. Firewall/NAT configuration
must allow the matching UDP endpoints in both directions. Rendezvous is not
a guarantee of traversal through arbitrary NATs. See [Rendezvous](rendezvous.md).
The two local-bind options are deliberately rejected outside Rendezvous mode;
for Listener mode, `--srt-host` / `--srt-port` already define the bind endpoint.

## Optional AES-CTR encryption

Set the same passphrase in an environment variable on both machines, then
add `--passphrase-env BRIDGE_SECRET` to both bridge commands. Its value must
contain 10–79 bytes. Pass only the variable's name on the command line, not
the secret itself. Use your shell's hidden-input mechanism or a credential
provider to populate it; do not paste real credentials into shared commands.

This selects AES-CTR with a 256-bit key. No passphrase means no encryption.
This bridge deliberately does not expose AES-GCM selection, even when built
with the extension enabled. AES-CTR does not authenticate payload integrity;
read [Security](../SECURITY.md) and the
[0.2.3 qualification limits](release-notes-0.2.3.md#qualification-limits-and-release-acceptance).
The independent cryptographic review remains outstanding.

## UDP multicast

Use an IPv4 multicast group as `--udp-host`. In `send` mode the bridge joins
that input group; in `receive` mode it transmits to that output group.
`--udp-interface` is the **local IPv4 address**, not an interface name or the
group address. Default `0.0.0.0` lets the OS choose. Select it explicitly on
multi-interface hosts. Input binds the wildcard address and joins the group
on this interface. Membership is released when the socket closes.

For example, join an existing source on the sending machine:

```sh
./build-bridge/robotweax_srt_udp_bridge send \
  --udp-host 239.10.20.30 --udp-port 5000 --udp-interface 192.168.10.20 \
  --srt-mode caller --srt-host 192.168.20.30 --srt-port 9000
```

On the receiving machine, output to another multicast group:

```sh
./build-bridge/robotweax_srt_udp_bridge receive \
  --srt-mode listener --srt-host 0.0.0.0 --srt-port 9000 \
  --udp-host 239.10.20.31 --udp-port 5001 --udp-interface 192.168.20.30 \
  --udp-ttl 1
```

Replace interface addresses with addresses assigned to the respective hosts.
`--udp-ttl` accepts 1–255, defaults to 1, and applies only to multicast output.
Multicast loopback retains the OS default; local subscribers can receive
output. Routers, IGMP snooping, firewalls and group membership must permit the
traffic. Source-Specific Multicast (SSM), interface names, IPv6 multicast and
RTP depacketization are not implemented. The application joins any-source
multicast; do not treat group membership as sender authentication.

## Live status and statistics

`--stats-interval-ms` controls the reporting interval (200–60000, default 1000).
`--input-idle-ms` controls input inactivity classification (200–60000, default
2000). Status continues even with no incoming datagrams. Reports can be delayed
by a bounded blocking send; this is not a hard-real-time sampling service.

- `connection=CONNECTED`, `input=WAITING|ACTIVE|IDLE` distinguish SRT state
  from application input activity. `input_kind=UDP` describes the sender's
  input; `input_kind=SRT` describes the receiver's input. An idle receiver
  cannot tell whether the remote UDP source stopped or traffic was lost.
- `input_payload_mbps` and `output_payload_mbps` are interval rates of
  **successfully forwarded payload bytes**, excluding UDP/IP/SRT headers.
  They are equal because the bridge does not transcode or repacketize.
  They are not interface/wire bitrate, encoder bitrate, or remote delivery
  confirmations; discarded packets before forwarding are not included.
- `forwarded_datagrams` and `forwarded_bytes` are cumulative for the current
  connection generation, as are the underlying SRT totals.
- `rtt_ms`, `retrans_total`, `snd_drop_total`, `rcv_drop_total` expose SRT RTT,
  retransmissions and final sender/receiver drops. In particular, gap/loss
  detection is not presented as final packet loss.
- `snd_buffer_bytes`, `rcv_buffer_bytes`, `snd_buffer_ms`, `rcv_buffer_ms`
  expose instantaneous buffer occupancy and duration.
- `snd_latency_ms` and `rcv_latency_ms` expose the SRT-reported effective
  TSBPD delays. They are not glass-to-glass latency. Fields irrelevant to a
  socket's media direction can be zero; see [Statistics](statistics.md).
- All SRT statistics use `srt_bistats(socket, &stats, 0, 1)`: no counter reset.
  Failed snapshots emit `stats_available=false` and omit SRT fields rather
  than fabricating zero values. No new protocol or telemetry API is used.

## Reconnect and recovery

Add `--reconnect on` to either or both endpoints to enable automatic retry
(default `off`). This works in Caller, Listener and Rendezvous roles. Tune
`--reconnect-delay-ms` (100–60000, default 1000) and
`--peer-idle-timeout-ms` (1000–60000, default 5000) if necessary. Both
Rendezvous peers must still overlap their connection attempts; use a
sufficient `--connect-timeout-ms` window.

States are `CONNECTING` (Caller/Rendezvous), `READY SRT listener` (waiting),
`CONNECTED`, `DISCONNECTED`, `RECONNECTING`, and `STOPPED`. A recovered
`CONNECTED` line contains an incremented `generation` and `recovery_ms`,
measured from detected failure to the bridge being ready again. It does not
include the unknown time before the failure was detected. Initial connection
retries also report this interval, with generation 1 on first success.

Transient no-server, timeout and disconnected errors can be retried.
Authentication/connection rejection, invalid configuration or MPEG-TS,
partial-message anomalies and local UDP errors remain fatal (exit 1).
Logs include the SRT error code, not the passphrase. A cable interruption
and a peer process restart are not reliably distinguishable from SRT error
codes alone; `input=IDLE` while connected is reported separately.

UDP input is bound/joined **only after SRT connects** and is closed on session
failure. Input during an outage is not buffered or replayed. There is no
promise of lossless restart: the datagram involved in a failed send has
unknown delivery status, and the number of UDP datagrams lost outside the
socket's lifetime is unknown. Retry never blindly resends that datagram.

## Payload and operational limits

- Numeric IPv4 unicast and any-source multicast. No hostname resolution,
  IPv6, Source-Specific Multicast, or broadcast output.
- Raw MPEG-TS only: each datagram must contain 1–7 complete 188-byte packets
  (188–1316 bytes), with `0x47` at every packet boundary. This is basic framing
  validation, not validation of the entire MPEG transport stream.
- 192/204-byte TS variants, RTP headers, empty/oversized/unaligned datagrams
  and bad sync bytes are rejected with a diagnostic and exit code 1. They
  are not truncated or automatically repacketized. The receiver validates
  incoming SRT Messages in the same way.
- SRT Live/Message mode, TSBPD enabled, configurable latency (default 120 ms).
  The bridge does not add a second pacing queue. Late SRT packets may be
  dropped according to the live transport policy; UDP itself is unreliable.
- No unbounded queue. SRT sends time out after one second. Reconnect is
  opt-in and can abandon queued live data; see the recovery policy above.
- Listener waiting is interruptible; Caller and Rendezvous connection attempts are bounded
  by `--connect-timeout-ms` (default 10000). Ctrl+C is a local stop request,
  not a reliable delivery acknowledgement or graceful protocol EOF.
- Caller `--stream-id` is optional. No listener-side authentication/routing
  policy is supplied by this demo; do not use Stream ID as authentication.
- External SRT peers must use compatible live Message framing and payload
  sizes. The bridge's automated test is Robotweax↔Robotweax, not an additional
  claim of qualification against every encoder, receiver or SRT library.

## Focused test

Configure a separate build with tests and examples enabled, and build the
bridge target. Then run:

```sh
ctest --test-dir build-bridge-tests -C Release \
  -R '^robotweax_srt_udp_bridge_loopback$' --output-on-failure
```

The test checks all seven datagram sizes byte-for-byte, both Caller/Listener
directions, Rendezvous with either sender or receiver started first, plain
and AES-CTR Rendezvous transfer, multicast input/output, active and idle status,
and sender/receiver restarts in all three SRT roles. Invalid UDP payloads are
rejected even with reconnect enabled. It does
not measure maximum throughput or simulate WAN loss. Python 3.10+ is required
when tests are enabled.
