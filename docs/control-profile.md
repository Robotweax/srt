# Reliable control-message profile

`SRTT_CONTROL` is a Robotweax-only, opt-in transport type for commands that
must arrive intact and in send order. It is independent of the low-latency,
FEC-only `SRTT_SENSOR` profile. Both endpoints must select it before bind or
connect:

```c
SRT_TRANSTYPE type = SRTT_CONTROL;
if (srt_setsockflag(socket, SRTO_TRANSTYPE, &type, sizeof(type)) == SRT_ERROR) {
    /* Handle the SRT error. */
}
```

Use `srt_sendmsg2`/`srt_recvmsg2` or `srt_sendmsg`/`srt_recvmsg` for discrete
commands. The simpler `srt_send`/`srt_recv` calls also retain message
boundaries in this profile. A successful send means the command entered SRT's
bounded send buffer; it is not an application-level execution acknowledgement.
The receiving application must still acknowledge execution if its workflow
requires that guarantee.

## Delivery contract

- Message API is enabled. FileCC supplies pacing, flow control, NAK recovery,
  and retransmission timeout recovery.
- TSBPD and too-late packet drop are disabled. Messages are released as soon
  as all earlier packet sequences needed for ordered delivery are present.
- The sender sets the wire `inorder` bit even when the caller passes the
  default `SRT_MSGCTRL.inorder=0`. A finite `msgttl` is rejected with
  `SRT_EINVALMSGAPI`; negative values mean no expiry.
- Periodic NAK and the DATA retransmission flag follow File-mode defaults.
  Their disabled values do not disable NAK/ARQ or RTO recovery.
- The profile defaults to a 180-second linger period so synchronous close can
  wait for queued sends to drain. Applications should wait for their own
  command acknowledgement before closing when execution matters.

The profile has no built-in FEC filter, and `SRTO_PACKETFILTER` cannot be
enabled while it is selected. It supports AES-CTR and, in a build with
`ENABLE_AEAD_API_PREVIEW=ON`, explicitly selected AES-GCM. GCM uses the existing
[authenticated DATA contract](aes-gcm-contract.md); the `control-v1` wire identity
and delivery rules are unchanged.

## Wire identity and compatibility

The HSv5 CONFIG congestion extension (type 6) carries the exact text
`control-v1`. Its transfer algorithm is the existing FileCC; the distinct
versioned name identifies the Control delivery contract. Both peers must
advertise it, and the responder must echo it. A `SRTT_FILE` peer, a peer with
a different profile, or an older peer that ignores the name cannot silently
establish a Control connection. Robotweax peers reject a profile mismatch
with `SRT_REJ_CONGESTION` (reason 13) when controller identity is the first
incompatible property. An earlier Message-API or packet-filter check can instead
reject with `SRT_REJ_MESSAGEAPI` (12) or `SRT_REJ_FILTER` (14). Older peers may
reject or time out; none of these cases establish a Control connection.

No DATA packet layout or existing `LIVE`/`FILE` behavior changes. Switching
the pre-bind `SRTO_TRANSTYPE` option back to `SRTT_LIVE` or `SRTT_FILE`
restores the normal bundle. `SRTT_CONTROL` has enum value 4; `SRTT_INVALID`
remains 2 and `SRTT_SENSOR` remains 3.

## Authenticated Control configuration

Use an extension-enabled installed CMake target or its matching pkg-config
metadata. Select `SRTT_CONTROL`, set `SRTO_CRYPTOMODE` to the `int32_t` value `2`,
and configure a passphrase before connecting. These options can be set in either
order. Keep `SRTO_ENFORCEDENCRYPTION=true`; disabling it for Control/GCM is an
invalid combination. Neither a missing secret nor an incompatible peer permits
plaintext DATA. After establishment require `SRTO_CRYPTOMODE == 2`; after data
exchange both `SRTO_SNDKMSTATE` and `SRTO_RCVKMSTATE` report `SRT_KM_S_SECURED`.
Key confirmation may initially be pending; see [encryption startup](encryption.md#key-rotation).
Do not export key material.

A carrier of 1316 bytes fits unfiltered GCM at the default MSS of 1500 for both
IPv4 (1440-byte plaintext ceiling per packet) and IPv6 (1420 bytes). At IPv6
MSS 1280 the ceiling is 1200 bytes, so a 1316-byte command is fragmented.
Control messages can span packets; unlike Live, `SRTO_PAYLOADSIZE` is a packet
ceiling rather than a whole-message limit. The maximum locally queueable command
is the available send-buffer packet count times its effective plaintext payload
size. Choose command sizes that also fit the peer's negotiated receive/flow
window; an incomplete message cannot be released to reclaim its receive slots.
A receive buffer passed to the API must fit the whole command. Larger application
objects should be split into bounded commands. Exact binary transfers of 1316
and 20000 bytes are covered by the direct socket regression.

`msgttl` must remain negative and the sender forces ordered delivery even with
`inorder=0`. For nonblocking sockets, use `SRTO_SNDSYN=false` and
`SRTO_RCVSYN=false`, retry on `SRT_EASYNCSND`/`SRT_EASYNCRCV`, and use epoll for
readiness. Check each call: send success confirms local buffering only.

The public Message demo provides a minimal bidirectional Caller/Listener:

```sh
# Both shells provide the same secret in SRT_CONTROL_SECRET.
robotweax_srt_message_demo listener --port 9000 --profile control \
  --crypto gcm --passphrase-env SRT_CONTROL_SECRET --nonblocking
robotweax_srt_message_demo caller --host 127.0.0.1 --port 9000 \
  --profile control --crypto gcm --passphrase-env SRT_CONTROL_SECRET \
  --message command --nonblocking
```

GCM authenticates DATA containing commands and application acknowledgements.
Transport ACK, NAK, shutdown and key-management controls are not thereby all
authenticated; their existing validation and denial-of-service limits remain.
Duplicate DATA within the connection does not execute an additional receive.
Fresh sockets use fresh crypto/reliability state, with no persistent execution
history. Applications must supply durable request identities and acknowledgements
when they need retry safety across reconnects or process restarts.
