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
enabled while it is selected. It supports the normal AES-CTR transport; the
optional AES-GCM preview does not currently support File/Message control mode.

## Wire identity and compatibility

The HSv5 CONFIG congestion extension (type 6) carries the exact text
`control-v1`. Its transfer algorithm is the existing FileCC; the distinct
versioned name identifies the Control delivery contract. Both peers must
advertise it, and the responder must echo it. A `SRTT_FILE` peer, a peer with
a different profile, or an older peer that ignores the name cannot silently
establish a Control connection. Robotweax peers reject a profile mismatch
with `SRT_REJ_CONGESTION` (reason 13); older peers may reject or time out.

No DATA packet layout or existing `LIVE`/`FILE` behavior changes. Switching
the pre-bind `SRTO_TRANSTYPE` option back to `SRTT_LIVE` or `SRTT_FILE`
restores the normal bundle. `SRTT_CONTROL` has enum value 4; `SRTT_INVALID`
remains 2 and `SRTT_SENSOR` remains 3.
