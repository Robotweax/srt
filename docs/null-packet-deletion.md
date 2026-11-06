# Null packet deletion (draft contract)

**Status: experimental implementation on `dev/null-packet-deletion`.** NPD is
available through the private options below, default OFF. Codec, socket policy,
Caller/Listener negotiation, encrypted DATA, FEC, retransmission, pacing input
accounting and counters are integrated. Identifiers remain provisional and
unregistered. This is not a released feature or a RIST compatibility claim.

Development remains on the feature branch. Merging to `main` requires explicit
maintainer approval after feature completion. Remaining release qualification
includes deployed MPEG-TS corpora, other platforms, and the broader
loss/reordering, rotation and retransmission combinations listed below.

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
deterministic recovery matrix covers 216 combinations: CTR/GCM, 16/24/32-byte
keys, row/column/matrix FEC, always/onreq/never ARQ policies, authenticated or
legacy parity, and compressed TS or maximum-size passthrough payloads. Each
combination drops one source, swaps adjacent source DATA, verifies every restored
byte and checks NPD counters. This does not exhaust multi-loss patterns or
rotation during reordered recovery. The codec tests cover 3,279 deletion/retention combinations and every prefix across
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
