# Live audio and video over multicast dispatch groups

`robotweax_srt_multicast` forwards MPEG-TS from one Listener sender to multiple
Caller receivers using only the public `<srt/srt.h>` API:

```text
Encoder or FFmpeg --local UDP--> multicast send
                                      |
                      one SRT DATA copy over UDP multicast
                                 /             \
                            receive A       receive B
                                |               |
                            local UDP       local UDP
                                |               |
                              player          player
```

Each receiver also has its own unicast SRT control and repair connection to
the sender. The sample preserves datagram boundaries and media bytes without
transcoding, remuxing, or adding an application header. FFmpeg is a separate
encoder/player process; the C++ executable has no FFmpeg library dependency.

This uses the experimental [multicast dispatch extension](connection-groups.md#multicast-dispatch-groups).
Build matching feature headers and library from source. Released v0.3.1
packages do not contain these options even though the branch retains that
product version.

## Build

From the checkout containing this sample:

```sh
cmake -S . -B build-multicast -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_BUILD_EXAMPLES=ON -DROBOTWEAX_SRT_BUILD_TESTS=OFF \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF -DROBOTWEAX_SRT_BUILD_TOOLS=OFF
cmake --build build-multicast --parallel --target robotweax_srt_multicast
```

The executable is `build-multicast/examples/applications/robotweax_srt_multicast`.
Multi-config generators add a configuration directory; build with
`--config Release`. Commands below use POSIX shell syntax and numeric IPv4.

## Local live demonstration

Use four terminals. Start the player, sender, and receiver before the encoder.
These commands select loopback explicitly and keep multicast on this host
with TTL 0. The sample runs for at most one hour here; Ctrl+C stops it sooner.
Its default duration is 20 seconds.

Start an independently installed ffplay:

```sh
ffplay -f mpegts 'udp://127.0.0.1:5001?fifo_size=8192&overrun_nonfatal=1'
```

VLC can instead open `udp://@127.0.0.1:5001`.

Start the sender, which waits for at least one receiver:

```sh
./build-multicast/examples/applications/robotweax_srt_multicast send \
  --endpoint 127.0.0.1:9000 --interface 127.0.0.1 \
  --multicast 239.255.133.11:10000 --ttl 0 \
  --media-udp 127.0.0.1:5000 --duration 3600
```

Wait for `"event":"ready"`, then start the receiver:

```sh
./build-multicast/examples/applications/robotweax_srt_multicast receive \
  --endpoint 127.0.0.1:9000 --interface 127.0.0.1 \
  --media-udp 127.0.0.1:5001 --duration 3600
```

Wait for **both** applications to print `"event":"media_ready"` before
starting the source. The sender then has its input UDP socket bound, and the
receiver has joined the multicast group. With encryption, the sender also
waits for the requested minimum number of receivers to acknowledge the key.

Generate a live test picture and audible 1 kHz tone. This command needs an
FFmpeg build with `libx264` and AAC encoding:

```sh
ffmpeg -hide_banner \
  -re -f lavfi -i 'testsrc2=size=640x360:rate=25' \
  -re -f lavfi -i 'sine=frequency=1000:sample_rate=48000' \
  -map 0:v:0 -map 1:a:0 \
  -c:v libx264 -preset veryfast -tune zerolatency -g 25 -pix_fmt yuv420p \
  -c:a aac -b:a 128k -muxdelay 0 -muxpreload 0 \
  -f mpegts 'udp://127.0.0.1:5000?pkt_size=1316'
```

Replace the generated inputs with your camera and microphone inputs for
capture. To replay an existing audio/video file in real time:

```sh
ffmpeg -re -i input.ts -map 0:v:0 -map 0:a:0 -c copy \
  -f mpegts 'udp://127.0.0.1:5000?pkt_size=1316'
```

Each datagram must contain 1 to 7 aligned 188-byte TS packets, with a sync byte
at each packet boundary. RTP and arbitrary byte chunks are rejected. The player
consumes the receiver's UDP output, where SRT headers have been removed. A late
joiner needs program tables, codec headers, and a usable keyframe from the
source before playback can begin.

## Additional receivers and recording

Start another player on port 5002, then run another receiver:

```sh
./build-multicast/examples/applications/robotweax_srt_multicast receive \
  --endpoint 127.0.0.1:9000 --interface 127.0.0.1 \
  --media-udp 127.0.0.1:5002 --duration 3600 --record received.ts
```

`--record` writes TS alongside UDP output and overwrites the explicitly
selected file. To require two receivers at startup, add `--min-receivers 2`
to the sender. This setting supports 1 to 16; it is a sample control, not a
qualified library fan-out size.

## Separate machines

The sender uses `--endpoint SENDER_LAN_IP:9000 --interface SENDER_LAN_IP`
and `--ttl 1`. The bind and interface addresses must match so the multicast
source matches the receiver's unicast peer. Each receiver uses
`--endpoint SENDER_LAN_IP:9000 --interface RECEIVER_LAN_IP`. Keep
`--media-udp 127.0.0.1:5001` for a player on that receiver's machine.

The network must carry IPv4 multicast, including IGMP membership and switch
snooping configuration. Permit the unicast control UDP port and multicast
data UDP port on the intended interfaces. Multicast routing beyond a LAN
requires network configuration beyond this sample. Local tests do not
qualify physical-network timing or scale.

## Encryption and admission

For AES-256-CTR, populate the same passphrase environment variable in the
sender and each receiver and add `--passphrase-env SRT_MEDIA_SECRET` to each
command. Pass the variable's name rather than the secret. For AES-GCM, build
with `-DENABLE_AEAD_API_PREVIEW=ON` and also pass `--crypto gcm` on both ends.
Cipher selections must match.

The example callback admits every multicast proposal into one fixed stream
and domain. Applications must replace this with their own authorization
policy. The group key design is unreviewed. Removing a receiver does not revoke
keys it already knows, and shared keys do not provide sender identity against
other key holders. See the [encryption contract](encryption.md#multicast-dispatch-groups-one-traffic-key-per-group).

The sender's `--repeat-threshold 0` default keeps repairs unicast. Values 1 to
16 enable the experimental shared-loss multicast repair policy.

## Observations and lifecycle

JSONL `media_progress` and `media_summary` events count local bytes and
datagrams. `multicast_stats` reports membership, multicast packets and bytes,
repairs, and receiver ingress counters. Sender counts are local observations,
not delivery acknowledgements. Originals should be sent once per input
datagram even with multiple receivers.

The listener stays open for later joins. After startup, sends with no receivers
advance the stream without delivery. Setup has a bounded timeout. There is no
automatic reconnect or MPEG-TS EOF message, and shutdown does not promise to
drain pending data. Stop the encoder first and allow the receiver latency
before stopping applications when preserving the end of a finite recording.

## Automated checks

With examples and tests enabled, `robotweax_srt_multicast_example_*` checks
exact UDP boundaries, exact recordings, and one multicast original per input
datagram with two receivers. It covers plain and CTR modes, plus GCM when built.

Set `ROBOTWEAX_SRT_SAMPLE_FFMPEG` to register `robotweax_srt_multicast_media_*`.
These checks run live FFmpeg H.264 video and AAC audio into a UDP observation
relay, which records and forwards the exact encoder datagrams to the sender.
They compare both receivers byte for byte with that input and decode both
audio and video with errors treated as failures:

```sh
cmake -S . -B build-multicast-tests -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_BUILD_EXAMPLES=ON -DENABLE_AEAD_API_PREVIEW=ON \
  -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF -DROBOTWEAX_SRT_BUILD_TOOLS=OFF \
  -DROBOTWEAX_SRT_SAMPLE_FFMPEG=/absolute/path/to/ffmpeg
cmake --build build-multicast-tests --parallel --target robotweax_srt_multicast
ctest --test-dir build-multicast-tests --output-on-failure \
  -R '^robotweax_srt_multicast_(example|media)_'
```

Tests require IPv4 loopback multicast. They establish local transport and
decoder behavior, not physical-LAN scale, A/V continuity under loss, or a
supported platform release.
