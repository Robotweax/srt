# Null packet deletion (draft contract)

**Status: experimental implementation on `dev/null-packet-deletion`.** NPD is
available through the private options below, default OFF. Codec, socket policy,
Caller/Listener negotiation, encrypted DATA, FEC, retransmission, pacing input
accounting and counters are integrated. Identifiers remain provisional and
unregistered. This is not a released feature or a RIST compatibility claim.

Development remains on the feature branch. Merging to `main` requires explicit
maintainer approval after feature completion. Remaining release qualification
includes deployed MPEG-TS corpora, physical-network pacing measurements, and
the broader loss/reordering, rotation and retransmission combinations listed below.

Null packet deletion (NPD) removes MPEG-TS null packets from live payloads at
the sender and restores them at the receiver, so constant-bitrate transport
streams do not spend bandwidth on stuffing. RIST Main Profile defines a feature
with the same purpose; this format is independently designed for SRT DATA
packets and is not interoperable with RIST.

## Compatibility boundary

NPD reuses the existing optional-extension policy values
(`ROBOTWEAX_SRT_EXTENSION_OFF`, `_REQUIRED`, `_IF_OFFERED`) introduced for
[path identifiers](group-path-identifiers.md). `IF_OFFERED` is listener-only.

| Caller | Listener | Result |
| --- | --- | --- |
| OFF or stock | OFF or stock | Unchanged handshake and DATA bytes |
| OFF or stock | IF_OFFERED | Ordinary SRT; the listener emits no record |
| OFF or stock | REQUIRED | Listener rejects setup |
| REQUIRED | IF_OFFERED or REQUIRED | NPD active in both directions |
| REQUIRED | OFF | Setup fails; the caller receives no echo |
| REQUIRED | Stock | Setup fails; interoperability is not claimed |

A listener never sends an NPD record to a caller that did not offer one. An
`IF_OFFERED` listener therefore serves stock callers with unchanged bytes by
construction, without relying on how a stock peer treats unknown records.

A stock listener that receives an offer may ignore it, reject it or time out.
The caller fails setup in every case.

*Open:* a caller-side fallback value that offers NPD and continues with
ordinary SRT when no echo arrives. It would be a new policy value, and it
depends on stock listeners skipping an unknown CONFIG record. The
[identity experiment](implementation-identity.md) observed that for two pinned
reference builds only.

## Application contract

When NPD is active the receiving application gets the same bytes, the same
message boundaries, original timestamps and TSBPD scheduling. This follows
from three rules:

- only *canonical* null packets are deleted, so restoration is bit-exact;
- every DATA packet is still sent, so sequence numbers, timestamps and TSBPD
  delivery are untouched; and
- payloads that are not aligned MPEG-TS pass through unmodified.

Private options:

| Option | Contract |
| --- | --- |
| `SRTO_ROBOTWEAX_NPD_POLICY` (`0x01000018`) | PRE `int32_t`, default OFF: OFF, REQUIRED or IF_OFFERED; inherited by accepted sockets, which report the negotiated result as REQUIRED or OFF |
| `SRTO_ROBOTWEAX_NPD_STATS` (`0x01000019`) | Read-only `ROBOTWEAX_SRT_NPD_STATS_V1`, cumulative, no clear-on-read; groups return `SRT_EINVOP` |

The stats structure is 32 bytes: two `uint32_t` fields (`abi_version`, always
`ROBOTWEAX_SRT_NPD_STATS_VERSION` = 1, and `active`, 0 or 1), followed by three
`uint64_t` fields: `deleted_units`, `restored_units`, `malformed_payloads`.
Deleted units count successfully queued messages, including messages later
dropped by the sender; restored units count uniquely accepted source packets,
including FEC recovery. Malformed counts failed NPD validation after decryption.
An unconnected socket returns version 1 with other fields zero. Supply at least
`sizeof(ROBOTWEAX_SRT_NPD_STATS_V1)`; undersized getters fail without a partial
write. These fields do not extend or change `SRT_TRACEBSTATS`.


A socket holding `IF_OFFERED` fails `srt_connect` with `SRT_EINVPARAM`, as it
does for path identifiers.

Version 1 is limited to individual Caller/Listener sockets using the Live
congestion controller with the message API. A non-OFF policy combined with
Rendezvous or File/Stream mode fails in either setting order, and group handles
return `SRT_EINVOP`. Rendezvous and groups are *open* follow-ups, not silent
exclusions.

A non-OFF policy reserves one byte from the maximum `SRTO_PAYLOADSIZE` ceiling,
as FEC reserves four. The default 1,316-byte profile is unaffected. The byte is
reserved before connection setup, so an `IF_OFFERED` listener also reserves it
for connections that negotiate OFF.

## Handshake record

The HSv5 CONFIG command is `0x7f15`. INDUCTION is unchanged. The record is
emitted only with a CONCLUSION that carries HSREQ or HSRSP, after the existing
private records in canonical order. Content is exactly 8 bytes (2 words):

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 4 | Literal ASCII `SNPD` |
| 4 | 1 | Format version, 1 |
| 5 | 1 | Flags, zero |
| 6 | 2 | Reserved, zero |

A REQUIRED caller offers the record. A REQUIRED or `IF_OFFERED` listener echoes
it byte for byte in its CONCLUSION response and commits to NPD for that
connection. The caller activates NPD only on an exact echo.

- With policy OFF NPD content validity is not enforced. The record is ignored
  like an unknown record; the enclosing extension chain must still be valid.
- A REQUIRED listener rejects a CONCLUSION without a valid offer.
- A REQUIRED caller that receives no echo fails setup and closes.
- Under REQUIRED, an echo that differs from the offer fails setup. An OFF
  caller ignores an unsolicited record, preserving unknown-record behavior.
- Under a non-OFF policy, wrong magic, an unknown version, nonzero flags or
  reserved bytes, a wrong size, a duplicate record or a missing CONFIG bit fail
  negotiation. The decoder retains a bounded validity flag so OFF can ignore
  invalid NPD content without weakening REQUIRED.

Offers and echoes are cached for handshake retries and cannot change within one
transaction. With [session authentication](session-authentication.md), the
caller repeats the record in AGREEMENT and the listener ignores inconsistent
retries, requiring a matching confirmation before publishing the connection. A handshake that cannot hold the record
within its MSS budget fails encoding; it is never truncated.

A missing or invalid required record is rejected with `SRT_REJ_ROGUE` (invalid
handshake parameters). The caller fails locally if the listener omits its echo.

## DATA payload format

When NPD is active, every source DATA packet in both directions carries a
one-byte prefix before encryption. Retransmissions repeat the original wire
payload. Packet-filter control packets, control packets and the DATA header are
unchanged.

| Bits of prefix | Meaning |
| --- | --- |
| 7 | Reserved, zero |
| 6–0 | Deletion bitmap; bit *i* set means unit *i* of the original payload was a deleted null packet |

A *unit* is 188 consecutive bytes. A *canonical null packet* is the unit
`47 1F FF 10` followed by 184 bytes of `FF`.

### Sender

1. If the application payload length is a multiple of 188, holds one to seven
   units, and every unit starts with `47`, the payload is *eligible*.
2. For an eligible payload the sender may delete any canonical null unit and
   set its bit. It may also keep a null unit, for example to hold a minimum
   wire rate.
3. For any other payload the bitmap is zero and the bytes are unchanged.
4. The wire payload is the prefix followed by the remaining bytes. A payload of
   seven deleted units is sent as the single byte `7F`.

Units with PID `0x1FFF` that are not canonical (a nonzero continuity counter,
an adaptation field, or any other fill) are never deleted in version 1.

### Receiver

Let `b` be the prefix and `rest` the bytes after it.

- If bit 7 of `b` is set, the payload is malformed.
- If `b` is zero, `rest` is delivered unchanged and must not be empty.
- Otherwise `rest` must be a multiple of 188. With `k = rest / 188` and
  `n = k + popcount(b)`, `n` must not exceed seven and the highest set bit of
  `b` must be below `n`. The receiver rebuilds `n` units in order: a canonical
  null packet where the bit is set, the next received unit otherwise.

Restoration happens after decryption and authentication and before application
delivery. A restored payload obeys the same local size rules as an ordinary
received payload of that length.

A malformed payload is handled like DATA that failed authentication: it is not
delivered or acknowledged, increments the private malformed counter and the
existing undecryptable counter, and does not refresh peer liveness. The
connection continues; subsequent DATA can expose the missing sequence to ARQ.

## Interaction with other features

- **Encryption.** The prefix is plaintext input to the cipher. AES-CTR
  encrypts it; AES-GCM also authenticates it. Key selection is unchanged.
- **FEC.** Parity is generated over the wire bytes, as described in
  [Packet filter](packet-filter.md), so it protects the shortened payload and
  its prefix. Let `P` be the effective maximum application payload after all
  MSS, address-family, NPD, FEC and encryption budget constraints, and let `T`
  be the DATA authentication-tag size (16 bytes for AES-GCM, zero for clear
  DATA or AES-CTR). The encoder's source capacity, decoder recovery buffers
  and parity XOR area must all accommodate `P + 1 + T` protected bytes.
  Parity payloads contain that full XOR area plus the existing four-byte FEC
  header and, when enabled, the session-authentication v2 parity overhead.
  These sizes must fit the negotiated MSS budget; increasing only the
  decoder buffer is insufficient. For an unchanged `P` of 1,316 bytes, the
  protected capacity is 1,317 bytes without GCM or 1,333 bytes with GCM,
  making parity one byte larger than without NPD. Parity size is fixed at
  this capacity even when source units are deleted, so NPD does not reduce
  FEC bytes.
- **ARQ.** Loss reports, ACKs and retransmission are unchanged.
- **Pacing.** The input-rate estimate keeps observing the application payload
  before deletion, while the average packet size observes wire payloads. The
  pacing budget therefore retains the application's nominal input rate after
  null runs. The prefix remains part of wire pacing cost; identical wall-clock
  delivery times are not guaranteed.
  Explicit `SRTO_MAXBW` and `SRTO_INPUTBW` values keep their meaning.
- **Statistics.** For negotiated NPD, source wire-byte counters count protected
  payload bytes, including the prefix and any GCM tag, plus their usual packet
  overhead. Delivered and receive-buffer payload fields count restored bytes.
  The private getter above reports deleted, restored and malformed counts.
- **Security.** NPD is not a protection feature. Session authentication and
  ACP1 do not authenticate the handshake transcript, so an on-path attacker can
  remove or forge the record. Removal fails setup. A forged echo leaves the
  peers disagreeing about the payload format and is not detected in version 1.

## Alternative considered

The [experimental compiled packet filters](experimental-filters.md) binding
could carry NPD as a named filter. It was not chosen for version 1: that
binding rejects encryption and session authentication, and it reserves 23
bytes of payload capacity for its own framing.

## Out of scope for version 1

- 204-byte transport packets and non-canonical null packets.
- Connection groups, multicast dispatch groups and Rendezvous.
- File and Stream modes.
- Smaller parity packets.
- Any compatibility with the RIST wire format.

## Reproducible development evidence

The focused `npd_` native cases exercise policy and option ordering, exact echo
and retries, authenticated AGREEMENT, clear/CTR/GCM transfers, authenticated
parity, all key lengths across the FEC geometries, key rotation, ARQ and FEC loss
recovery, payload ceilings and preservation of the input-rate estimate. The
deterministic recovery matrix covers 288 combinations: CTR/GCM, 16/24/32-byte
keys, row/column/matrix FEC, always/onreq/never ARQ policies, authenticated or
legacy parity, and compressed TS or maximum-size passthrough payloads. Each
combination drops one source, swaps adjacent source DATA, verifies every restored
byte and checks NPD counters. This includes 72 matrix-FEC cases spanning an
explicitly acknowledged even-to-odd key change, with five old-key sources and
four new-key sources. It does not exhaust multi-loss patterns, delayed key
announcements or repeated rotations during recovery.

A further 18 ARQ cases span CTR/GCM, every key size and all-null, partially
compressed or passthrough payloads. They drop an old-key source, deliver newer
sources in reverse order across a key change, and then request the old source.
The repair must preserve the original encryption-key selector and ciphertext
(including the GCM tag), restore ordered bit-exact delivery, and leave counters
unchanged on late originals or duplicate repairs. Key exchange is explicitly
acknowledged in these deterministic fixtures; it does not simulate key-message
loss.

A separate 24-case delayed-response matrix covers CTR/GCM, all three key sizes,
30 ms and 100 ms key-response delays, and either delivery or loss of the first
response. Each case queues 1,200 messages alternating all-null, partially
compressed and full-size passthrough payloads, then drains the queue. It checks
ordered bit-exact delivery, repeated key changes, exact deletion/restoration
counts, zero malformed or undecryptable packets, and no too-late drops. The
100 ms cases must exercise the sender's bounded key-use pause. These fixtures
delay rotation responses after successful setup; they do not combine delayed
key exchange with DATA loss, FEC or network reordering. The codec tests cover 3,279 deletion/retention combinations and every prefix across
payload lengths through 1,504 bytes. The dedicated fuzz targets are
`fuzz_null_packet_deletion` and `fuzz_handshake_decode`.

```sh
cmake -S . -B build-npd -DCMAKE_BUILD_TYPE=Debug -DENABLE_AEAD_API_PREVIEW=ON
cmake --build build-npd --parallel
ctest --test-dir build-npd -R robotweax_srt_npd_tests --output-on-failure
./tools/python interop/run_npd_interop.py \
  --robotweax-peer build-npd/robotweax_srt_interop_peer \
  --reference-peer /path/to/pinned-ctr-peer \
  --reference-peer /path/to/pinned-aead-peer \
  --output /tmp/npd-qualification
```

The runner records commands, binary SHA-256 hashes, source/parity byte counts,
DATA payload hashes and handshake extension records. Comparisons exclude
per-connection socket IDs, timestamps, cookies and initial sequence numbers.
The deterministic corpus contains canonical nulls, real TS-shaped packets and
non-canonical nulls; it is not a capture of a deployed broadcast stream.

A local macOS/OpenSSL run passed 20 separate-process cases using the pinned
Haivision public-API peers at `899348d8318eb9a3c5a5b6ec43c4a1114288773a`
(CTR) and `d99d2e1a3b1a213b03c7dfbea5133898935fdeea` (AEAD). No reference
implementation source was inspected. Stock callers saw identical DATA payloads
and handshake extension records from OFF and IF_OFFERED listeners in both data
directions. Both references skipped the unknown offer and omitted an echo;
REQUIRED callers rejected that response. REQUIRED listeners rejected stock
callers without an offer.

A local macOS/MbedTLS 3.6.7 build also passed the focused NPD suite and all
20 black-box cases against the same pinned public-API peers. The encrypted
recovery matrix passed with both providers. Reproduce the MbedTLS configuration
by adding `-DROBOTWEAX_SRT_CRYPTO_BACKEND=mbedtls` and
`-DMbedTLS_ROOT=/path/to/mbedtls-3.6.7-install` to the CMake command above.
The separate-process runner records each candidate binary hash in `report.json`;
its current wire comparisons use clear DATA, while native tests exercise both
cipher modes. These results cover local macOS builds, not other platforms.

For the 84,224-byte synthetic corpus, source DATA payload bytes fell from
84,224 to 45,184 in each direction. With row FEC, parity payload bytes increased
from 21,120 to 21,136 (16 parity packets), confirming the added prefix capacity.
These are payload-byte measurements, not an end-to-end network bandwidth claim.

### Supplied streams and burst scheduling

Pass `--input /path/to/clip.ts` to qualify a short supplied stream instead of
the deterministic corpus. The runner reads 1,316-byte application messages,
retains a partial final message, checks bit-exact delivery in both directions
with and without row FEC, and compares source bytes against an independent
length oracle. It records the input SHA-256 and application byte count. A
stream without canonical nulls is valid: its NPD source size grows by one byte
per message. Use short clips that complete within the runner's eight-second
process timeout. Input/output copies remain in the evidence directory; do not
commit private captures.

A local FFmpeg 9.0.2 MPEG-2/TS run passed all eight self-peer cases:

```sh
ffmpeg -f lavfi -i testsrc2=size=160x90:rate=25 -t 1 \
  -c:v mpeg2video -b:v 100k -muxrate 1000000 -f mpegts /tmp/npd-cbr.ts
./tools/python interop/run_npd_interop.py \
  --robotweax-peer build-npd/robotweax_srt_interop_peer \
  --input /tmp/npd-cbr.ts --output /tmp/npd-cbr-evidence
```

The generated clip had 122,764 bytes (SHA-256
`a8d27a52288547f2949ed6e1d98383a782536be63e593aa06113425968758abf`).
NPD source DATA payloads totaled 51,982 bytes in each direction; row-FEC parity
payloads changed from 30,360 to 30,383 bytes. This is a generated muxed stream,
not a deployed broadcast capture, and counts payload bytes rather than total
network traffic. Different FFmpeg versions can produce different input bytes.

The deterministic `npd_burst_after_null_run_preserves_sender_schedule` test
sends sixteen all-null messages, then queues eight full-size passthrough
messages. It compares actual pacer release deadlines with NPD OFF, permitting
only the extra prefix's serialization time. This checks the sender scheduling
model; operating-system wake-up and end-to-end delivery jitter still require
platform measurements.

### Linux and released OFF/OFF comparison

The Linux qualification uses Ubuntu 26.10 ARM64 on Docker/LinuxKit, GCC 15.3.0,
OpenSSL 4.0.3, and a Release build with AEAD preview and warnings-as-errors.
The source revision is `0906db09a3bceeae626ca7eaab225bd6fb9a89c4`; the released
baseline is v0.3.2 at `e45d0edd346e5c427d85e4c3b1ac605c81a23397`.
The Ubuntu image is pinned to
`sha256:93e29beff5257b2cdf1c024d831f6d2997e53655a653ff76a811fc9e6564a719`,
also used by the repository's GCC 15 CI job.

All 100 CTest checks passed across the full run and an isolated package-consumer
rerun after installing the static-link dependencies below. This includes the
20 focused NPD native cases, the 855-case Python harness (with platform-specific
skips), and the public-API self-peer wire test. All 16 separate-process
qualification cases passed, including the eight released OFF/OFF comparisons.
These results qualify this Linux ARM64 container environment; they do not cover
native Windows, physical network interfaces or deployed broadcast streams.

Inside that image, install `g++-15 cmake make libssl-dev python3 git pkg-config`
and `libjitterentropy3-dev zlib1g-dev libzstd-dev`. The latter packages are
needed by the OpenSSL 4 static-link package-consumer check.
Use an actual Git checkout: the CI-classification tests inspect its tracked
tree. Configure and run the candidate with:

```sh
CC=gcc-15 CXX=g++-15 cmake -S . -B build-npd-linux \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_AEAD_API_PREVIEW=ON \
  -DROBOTWEAX_SRT_WARNINGS_AS_ERRORS=ON \
  -DROBOTWEAX_SRT_BUILD_TOOLS=ON -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF
cmake --build build-npd-linux --parallel 4
ctest --test-dir build-npd-linux --parallel 2 --output-on-failure
```

Build `robotweax_srt_interop_peer` from the pinned release in a separate checkout,
using the same compiler, provider and AEAD configuration. Then run:

```sh
./tools/python interop/run_npd_interop.py \
  --robotweax-peer build-npd-linux/robotweax_srt_interop_peer \
  --baseline-peer /path/to/v0.3.2-build/robotweax_srt_interop_peer \
  --output /tmp/npd-linux-wire
```

`--baseline-peer` adds eight transfer cases: released versus candidate OFF/OFF,
in both directions, with and without row FEC. It requires identical clear DATA
payload hashes, source/parity payload byte totals and handshake extension
records. Dynamic header fields remain excluded as described above. The report
records both binary hashes. This comparison does not claim byte-for-byte
identity of encrypted sessions or operating-system timing.

### Native Windows qualification

[Windows qualification run 42222253159](https://github.com/Robotweax/srt/actions/runs/42222253159)
passed on source `d2a3ca2a3748171d426375ef33cfcdff0ae20410`:

| Native target | Provider | Result |
| --- | --- | --- |
| Windows Server 2025 x64 | BCrypt | 21 NPD, 166 runtime and 3 rotation cases; 8 wire cases passed |
| Windows Server 2025 x64 | OpenSSL 3.6.5 | 21 NPD, 166 runtime and 3 rotation cases; 8 wire cases passed |
| Windows 11 ARM64 | BCrypt | 21 NPD, 166 runtime and 3 rotation cases; 8 wire cases passed |

These were Release builds with MSVC 19.51.36260 and AEAD preview enabled.
This evidence covers the listed configurations and loopback traffic; it does
not establish Win32, Debug, Windows ARM64/OpenSSL or physical-network timing
qualification.

The existing Windows backend workflow has a focused `npd_only` manual mode:

```sh
gh workflow run windows-bcrypt.yml --repo Robotweax/srt \
  --ref dev/null-packet-deletion -f npd_only=true -f qualification=false
```

It selects native x64 BCrypt, native x64 OpenSSL and native ARM64 BCrypt
Release builds, enables AEAD preview, and verifies that runner and target
architectures match. It runs the NPD, rotation and transport-runtime native
partitions plus eight separate-process wire cases per configuration. Artifacts
retain the tested commit, architecture, CMake dependency details, native logs,
wire reports and binary hashes for 14 days. The other backend workflow jobs
are skipped in this mode; repository permissions remain read-only.

The first Windows run exposed a test-fixture pacing assumption: one poll did
not always drain source DATA queued behind parity before the fixture announced
a successor key. The fixture now waits for captured source packets and checks
the key's packet count before announcing. Recovery, byte equality and key-selector
assertions are unchanged.

### SRT Network Lab integration assessment

The Lab was reviewed at `127001108e35b1d9cdc4da14c9d85d09143043ab`.
Its [contribution-media profile](https://github.com/Robotweax/srt_network_lab/blob/127001108e35b1d9cdc4da14c9d85d09143043ab/docs/contribution-media-lab.md)
provides isolated kernel-netem routers, source/receiver/observer recordings,
packet captures, per-datagram timing, source pins and independent validation.
Those mechanisms are suitable foundations for a separate NPD profile.

The existing profile cannot qualify NPD unchanged. Its
[endpoint adapter](https://github.com/Robotweax/srt_network_lab/blob/127001108e35b1d9cdc4da14c9d85d09143043ab/srt_network_lab/contribution_media.py)
uses Backup/Broadcast groups, which this NPD contract rejects. Its
[source generator](https://github.com/Robotweax/srt_network_lab/blob/127001108e35b1d9cdc4da14c9d85d09143043ab/scripts/run_docker_contribution_media.py)
creates a synthetic H.264/AAC test pattern without an explicit constant mux
rate. The reviewed tracked tree contains no MPEG-TS media file or NPD profile;
the documented media fixtures are generated, not deployed broadcast captures.
This finding does not inventory private or externally retained captures.

The assessment recommended a separate Caller/Listener profile, preserving
the existing frozen group matrix and its validators:

- Build ordinary public-API peers from an immutable NPD feature commit. Compare
  OFF/OFF with REQUIRED/IF_OFFERED using identical application bytes and offered
  rates, and record the resolved policy and NPD statistics at both ends.
- Accept a hashed operator-supplied clip, or generate an explicitly labelled
  constant-rate MPEG-TS source with a measured canonical-null inventory. Include
  null runs followed by full-size payload bursts; record source pacing.
- Reuse the isolated router and capture infrastructure for separately declared
  baseline and impairment cases. Retain original failures and verify cleanup.
- Independently verify restored bytes and ordering, prefix/deletion counts,
  DATA/FEC/ARQ wire-byte totals, capture completeness and measured sender and
  receiver timing. Declare measurement thresholds before executing the matrix;
  an internally valid archive alone is not an NPD acceptance result.

The Lab's [two-host executor](https://github.com/Robotweax/srt_network_lab/blob/127001108e35b1d9cdc4da14c9d85d09143043ab/docs/contribution-two-host-baseline.md)
is also group-based and needs supplied endpoint configuration; it is not a
ready-made NPD physical-network test. This assessment changed no Lab code and
launched no Lab or cloud workload. Docker logical-path evidence and a genuine
deployed-stream corpus remain distinct qualification requirements.

### Subsequent local Lab qualification

[Lab PR #387](https://github.com/Robotweax/srt_network_lab/pull/387) implements
that separate profile on `srt/npd-qualification`; it remains a draft and is not
merged. The [retained results](https://github.com/Robotweax/srt_network_lab/blob/cf9321fdf3d575ae37b1e23f688d2761e7cfd14a/results/2026-11-06-npd-qualification/README.md)
qualify SRT commit `b06cde38704e21d2c4a152343ca972fe5b1dda9a` in a fixed,
unencrypted 16-case Docker kernel-netem matrix: both Caller/Listener data
directions, OFF versus negotiated NPD, no FEC versus row FEC, and baseline
versus loss/reordering. All 16 cases and eight paired comparisons passed the
independent evidence checks, including bitexact restoration, strict media
decode, observed drops/reordering, wire encoding and application/wire pacing.

The synthetic CBR source includes a canonical-null run and a retained-payload
burst. Original SRT data bytes, including the 16-byte SRT header but excluding
FEC, retransmissions and control traffic, fell from 1,068,264 to 633,470
(40.7%). This stuffing-dependent result does not predict deployed savings.
The largest paired increase in maximum delivery delay was 8.09 ms, within the
predeclared 100 ms bound. An initial invalid engineering run is retained and
documented; it exposed an oversized implicit UDP buffer request in the Lab
peer, corrected by explicitly requesting 1 MiB without changing host settings.

This is additional synthetic, single-host Linux Docker evidence. Release
acceptance remains unset; deployed-stream captures, physical hosts and the
other qualification requirements below remain separate. No main branch was
merged and no cloud resources were provisioned.

A [subsequent run with stronger capture and netem validation](https://github.com/Robotweax/srt_network_lab/blob/45cdeb24feba81277fa1b7ebb722e3b5c56a1675/results/2026-11-06-npd-qualification/README.md#stronger-validator-and-attempt-003)
is incomplete: 14/16 cases and 6/8 comparisons passed. One case did not
exercise reordering. In another, the final source packet was absent from the
receiver-side capture, was not retransmitted, and only 801/802 messages were
delivered before closure. This has not been attributed specifically to NPD;
a deterministic final-packet-loss comparison with NPD OFF and ON is needed.
The failed run is retained, no cases were replaced, and the Lab PR remains
draft. The earlier successful run does not close this later qualification gap.

The subsequent [32-case deterministic tail-loss diagnosis](https://github.com/Robotweax/srt_network_lab/blob/cea13e0ffe1282d7a787b2c4a3d7dc6c31b87721/results/2026-11-06-npd-tail-diagnostic/README.md)
found matching completion and delivered-message counts in all 16 OFF/ON pairs,
in both data directions. Interior loss recovered through ARQ; an uncovered
lost final packet remained missing with either NPD policy; a complete final
four-packet FEC row repaired its lost last packet. This isolates shared
finite-stream tail-loss behavior under the tested settings, not an NPD-specific
regression. The peer, shutdown grace and transport defaults were unchanged.
Diagnostic validity does not imply successful stream delivery: 20 recoverable
controls completed and 12 deliberately uncovered tails remained incomplete.
The original kernel-netem qualification remains incomplete, and any revised
finite-stream completion workload must have its own declared contract.

A separate [bounded application-completion experiment](https://github.com/Robotweax/srt_network_lab/blob/3f92fbc8d2683f2d7d9ef59065796cd7cecdd6e2/docs/npd-completion.md)
now has its own source-built peer and 48-case contract. Repeated end markers
and length/SHA256 receipts recovered all 32 declared positive cases, including
last-media, end-marker and receipt loss. All 16 negative controls prevented
sender success when receipts were blocked or a marker digest was corrupted.
The peer deadline is five seconds with at most 32 end markers. Both peers must
understand this application protocol; it does not change raw MPEG-TS behavior,
SRT defaults, or the original peer and failed matrix. Deployed captures and release acceptance remain separate work.


The subsequent [separate media-completion v2 qualification](https://github.com/Robotweax/srt_network_lab/blob/7b3fcbc3147ef9816887d4f4fa61cea5bb4cee83/results/2026-11-06-npd-media-v2/README.md)
passed all 16 kernel-netem cases and eight OFF/ON pairs with generated media.
Every stream was bit-exact and decoded successfully, and every impaired case
observed loss and reordering. Content receipts arrived within 667 ms of the
scheduled last media message, inside the five-second window. Original media
SRT DATA bytes fell 40.7%, with end markers, receipts and parity accounted
separately. The initial attempt remains invalid due to an incorrect validator
bound for reverse parity; the corrected full attempt is separately sealed.
This requires completion-aware peers and does not qualify deployed captures,
physical WANs, encryption or arbitrary raw-TS receivers. The original v1
incomplete results remain unchanged, and neither main branch was merged.

## Qualification required before broader support claims

Evidence must be reproducible and black-box, as required by
[Contributing](../CONTRIBUTING.md).

- Byte comparison showing that a stock caller sees the same handshake and DATA
  bytes from an `IF_OFFERED` listener as from an OFF listener.
- Byte comparison showing that OFF/OFF connections are unchanged from the
  current release.
- Explicit failure of REQUIRED against each pinned reference build in both
  roles, recording whether the reference ignores, rejects or times out.
- A self-peer matrix over AES-CTR and AES-GCM, every key length, key rotation,
  FEC geometries, FEC policies and ARQ levels, under loss, reordering and
  retransmission.
- Bit-exact delivery over a transport-stream corpus that includes
  non-canonical nulls, unaligned payloads, all-null payloads and lost sync.
- Fuzzing of the record parser and the prefix parser.
- A pacing comparison showing no added sender delay for bursts after null runs.
- Measured bandwidth saving on real constant-bitrate streams, with and without
  FEC.
- Relay probes for an unknown `0x7f15` record against each reference build, if
  the caller-side fallback is adopted.
