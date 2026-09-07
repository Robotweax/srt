# Public API examples

The examples in this directory are small, inspectable demonstrations of the
installed Robotweax SRT C API. They include only `<srt/srt.h>` and link through
the public `RobotweaxSRT::srt` target. They are not production relay tools and
do not replace application-specific reconnect, credential, or monitoring
policy.

## Live MPEG-TS UDP ↔ SRT bridge

The [UDP/SRT bridge guide](../docs/udp-srt-bridge.md) shows how to build and run
`robotweax_srt_udp_bridge send` and `robotweax_srt_udp_bridge receive` as a
continuous MPEG-TS UDP → SRT → UDP path. It preserves datagram boundaries and
supports IPv4 unicast, Caller/Listener and Rendezvous in either media direction,
configurable latency, optional AES-CTR, UDP multicast input/output, live SRT
statistics and opt-in reconnect. It does not transcode or promise lossless restart.

## Build the networked examples

The four networked example executables are opt-in and are not installed with
the library. The standalone consumer source described below is installed as
part of the public documentation package.

```sh
cmake -S . -B build-examples \
  -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_BUILD_EXAMPLES=ON \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
cmake --build build-examples \
  --target robotweax_srt_message_demo robotweax_srt_file_demo \
    robotweax_srt_group_demo robotweax_srt_udp_bridge --parallel
```

Multi-config generators place the executable in a configuration directory and
require `--config Release` when building. All four executables depend only on
the installed public SRT interface.

## Installed package consumer

The [standalone installed consumer](installed-consumer/README.md) is a small
downstream CMake project intended to be copied or adapted by integrators. It
uses only `<srt/srt.h>`, discovers an already installed package with
`find_package(RobotweaxSRT 0.2 CONFIG REQUIRED)`, and links
`RobotweaxSRT::srt`. The package-consumer CI test installs Robotweax SRT into
an isolated prefix and then configures, builds, and runs this exact public
source from the staged installation.

Use the standalone consumer to validate package discovery and runtime loading.
Use the networked examples below for complete Caller, Listener, Rendezvous,
File, encryption, FEC, and Connection Group flows.

## Message API demo

The demo supports Caller, Listener, and Rendezvous roles over numeric IPv4 or
IPv6 addresses. It sends one Message API payload, validates the peer response,
prints a non-destructive `srt_bistats(socket, stats, 0, 1)` snapshot, and
performs an orderly application-level shutdown.

### Caller and Listener

Start a Listener in the first terminal:

```sh
./build-examples/robotweax_srt_message_demo listener \
  --bind 127.0.0.1 \
  --port 9000 \
  --expect-stream-id demo/message
```

After it prints `READY`, run the Caller in another terminal:

```sh
./build-examples/robotweax_srt_message_demo caller \
  --host 127.0.0.1 \
  --port 9000 \
  --stream-id demo/message \
  --message "hello over SRT"
```

Add `--nonblocking` to both commands to exercise epoll-driven connect, accept,
send, and receive paths. The default timeout is 5000 milliseconds and can be
changed with `--timeout-ms`.

### Rendezvous

Rendezvous peers must use different local UDP ports and point to each other.
Start both commands close together in separate terminals:

```sh
./build-examples/robotweax_srt_message_demo rendezvous \
  --bind 127.0.0.1 --local-port 9001 \
  --host 127.0.0.1 --port 9002 \
  --message left --expect-message right
```

```sh
./build-examples/robotweax_srt_message_demo rendezvous \
  --bind 127.0.0.1 --local-port 9002 \
  --host 127.0.0.1 --port 9001 \
  --message right --expect-message left
```

Real deployments must account for NAT traversal, firewall policy, and
simultaneous connection timing. See the [Rendezvous guide](../docs/rendezvous.md).

### AES-CTR without exposing the passphrase

The demo deliberately does not accept a passphrase value on the command line,
where it could be retained in shell history or exposed in a process listing.
Set the same environment variable for both peers and pass only its name:

```sh
export ROBOTWEAX_SRT_DEMO_PASSPHRASE='replace-with-a-secret-value'

./build-examples/robotweax_srt_message_demo listener \
  --port 9000 \
  --passphrase-env ROBOTWEAX_SRT_DEMO_PASSPHRASE \
  --pbkeylen 32 --crypto ctr
```

```sh
export ROBOTWEAX_SRT_DEMO_PASSPHRASE='replace-with-a-secret-value'

./build-examples/robotweax_srt_message_demo caller \
  --port 9000 --message encrypted \
  --passphrase-env ROBOTWEAX_SRT_DEMO_PASSPHRASE \
  --pbkeylen 32 --crypto ctr
```

Environment variables are safer than literal command-line arguments for this
demonstration, but they are not a general secret-management system. Production
applications should use their platform's credential provider and minimize the
secret's lifetime in process memory.

### AES-GCM extension

AES-GCM is available only when the library and demo are built together with
the released Robotweax 0.2 extension enabled:

```sh
cmake -S . -B build-examples-gcm \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_AEAD_API_PREVIEW=ON \
  -DROBOTWEAX_SRT_BUILD_EXAMPLES=ON \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
  -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
cmake --build build-examples-gcm \
  --target robotweax_srt_message_demo robotweax_srt_file_demo \
    robotweax_srt_group_demo --parallel
```

Use `--crypto gcm` with `--passphrase-env` on both peers. A default build
rejects `--crypto gcm` rather than silently falling back to AES-CTR. Do not mix
default and GCM-enabled headers or binaries. Read the
[AES-GCM extension contract](../docs/aes-gcm-contract.md) before integration.

In all three demos, `--crypto ctr` (the default mode) and `--crypto gcm` are
explicit selections, not AUTO negotiation. A GCM-enabled demo sets
`SRTO_CRYPTOMODE` to 1 for CTR or 2 for GCM before connecting or listening;
groups propagate that selection to their member sockets. Encrypted peers
with different selections must reject the connection rather than transfer
payload. Selecting CTR does not by itself enable encryption: configure
`--passphrase-env` on both peers as shown above.

When examples change, CI tests matching CTR and GCM modes and rejects mixed
modes in both directions on Linux, macOS, and Windows. The group demo prints
per-member handshake rejection reasons when group connection setup fails.

### Packet-filter FEC

Pass the same compatible filter configuration to both peers:

```sh
--packet-filter 'fec,cols:10,arq:onreq'
```

FEC reduces the available payload size and adds recovery traffic. The demo
proves option negotiation and ordinary payload exchange; controlled loss and
recovery qualification belongs in a network test harness. See the
[packet-filter guide](../docs/packet-filter.md).

## File/Stream API demo

`robotweax_srt_file_demo` transfers one file per connection in File mode. It
uses Stream API loops for a small length-coded control header and orderly
shutdown, and the public blocking `srt_sendfile`/`srt_recvfile` helpers for the
payload. The receiver overwrites its output path and rejects a declared size
above `--max-bytes` before opening the file. This framing is a demo-specific
application protocol, not an additional SRT wire-protocol contract.

### Caller and Listener

Start a receiving Listener:

```sh
./build-examples/robotweax_srt_file_demo listener \
  --bind 127.0.0.1 --port 9000 \
  --output received.bin --max-bytes 1073741824 \
  --expect-stream-id demo/file
```

After it prints `READY`, send a file from the Caller:

```sh
./build-examples/robotweax_srt_file_demo caller \
  --host 127.0.0.1 --port 9000 \
  --input source.bin --stream-id demo/file
```

`--block-size` controls the reusable application buffer used by the file
helper; it does not define SRT packet boundaries. File helpers are blocking,
so applications that require event-loop cancellation or custom storage should
use partial Stream API loops instead. See the
[File/Stream mode guide](../docs/file-mode.md).

### Rendezvous

Choose the transfer direction by assigning `--input` to one peer and
`--output` to the other:

```sh
./build-examples/robotweax_srt_file_demo rendezvous \
  --bind 127.0.0.1 --local-port 9001 \
  --host 127.0.0.1 --port 9002 --input source.bin
```

```sh
./build-examples/robotweax_srt_file_demo rendezvous \
  --bind 127.0.0.1 --local-port 9002 \
  --host 127.0.0.1 --port 9001 \
  --output received.bin --max-bytes 1073741824
```

Start both peers close together. Production deployments must supply their own
retry, resumability, authentication, atomic-output, and file-name policy.

### Encryption

The File demo uses the same `--passphrase-env`, `--pbkeylen`, and
`--crypto ctr|gcm` options as the Message demo. Both peers must select the same
profile. AES-GCM requires `ENABLE_AEAD_API_PREVIEW=ON`; a default build rejects
it explicitly. GCM with File mode and packet-filter FEC is not supported.

## Connection Group API demo

`robotweax_srt_group_demo` demonstrates the public Live/Message Connection
Group API with two or more Caller/Listener connections. The Caller prepares a
bounded endpoint configuration, calls `srt_connect_group`, waits for every
member through `srt_group_data`, and transfers messages with group metadata in
`SRT_MSGCTRL.grpdata`. Both peers print non-destructive per-member
`srt_bistats` snapshots before shutdown.

Connection Group handles have fixed Live/Message semantics. Do not set
`SRTO_TRANSTYPE`, `SRTO_MESSAGEAPI`, or Stream ID on a group handle or endpoint
configuration. Set group-owned I/O timeouts on the group and only supported
member options, such as `SRTO_CONNTIMEO`, in the endpoint configuration.

### Broadcast

Start a group-enabled Listener:

```sh
export ROBOTWEAX_SRT_GROUP_PASSPHRASE='replace-with-a-secret-value'

./build-examples/robotweax_srt_group_demo listener \
  --bind 127.0.0.1 --port 9000 \
  --policy broadcast --members 2 \
  --passphrase-env ROBOTWEAX_SRT_GROUP_PASSPHRASE --pbkeylen 32
```

After it prints `READY`, run the Caller in another terminal with the same
environment variable:

```sh
./build-examples/robotweax_srt_group_demo caller \
  --host 127.0.0.1 --port 9000 \
  --policy broadcast --members 2 \
  --message "broadcast payload" \
  --passphrase-env ROBOTWEAX_SRT_GROUP_PASSPHRASE --pbkeylen 32
```

### Weighted Backup and deterministic failover

Use `--policy backup` on both peers. The demo assigns descending weights so
the first connected member is preferred. With `--failover-after 1`, the
Listener closes its active member after the first message; both peers then
prove continued delivery through the replacement, and the Caller requires an
`SRT_EPOLL_UPDATE` membership event:

```sh
./build-examples/robotweax_srt_group_demo listener \
  --bind 127.0.0.1 --port 9000 \
  --policy backup --members 2 --failover-after 1
```

```sh
./build-examples/robotweax_srt_group_demo caller \
  --host 127.0.0.1 --port 9000 \
  --policy backup --members 2 --failover-after 1 \
  --message "backup payload"
```

The local example connects all members to one Listener address and port. It
demonstrates public API, selection, update, and replay behavior, but it does
not prove independent interfaces, networks, or failure domains. Production
applications should use distinct paths or an explicit listener bond and
qualify failover on their deployment topology. See the
[Connection Group guide](../docs/connection-groups.md).

The Group demo accepts the same passphrase-environment, key-length, and crypto
options as the other examples. GCM requires `ENABLE_AEAD_API_PREVIEW=ON` on
both peers.

## Automated smoke tests

When examples and tests are both enabled, CTest registers deterministic local
Caller/Listener, Rendezvous, and Connection Group smoke tests:

```sh
ctest --test-dir build-examples -L example --output-on-failure
```

The Message Caller/Listener scenario covers Stream ID, AES-CTR, packet-filter
option negotiation, nonblocking epoll operation, exact payload echo,
statistics, and orderly shutdown. Its Rendezvous scenario exchanges and
validates one message in each direction. The File scenarios compare a
deterministic binary payload byte-for-byte after `srt_sendfile`/`srt_recvfile`
transfer with deliberately non-packet-sized helper buffers. The Group tests
cover encrypted Broadcast delivery plus a weighted Backup transition with
continued payload integrity and `SRT_EPOLL_UPDATE`. GCM-enabled builds add
encrypted Caller/Listener smoke tests for Message, File, and Broadcast Group
operation.
