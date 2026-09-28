# Early NAK diagnosis in encrypted FEC Rendezvous

The scheduled CI run [36406240853](https://github.com/Robotweax/srt/actions/runs/36406240853)
failed on `aes256-row-fec-rendezvous-haivision-to-robotweax-burst`.
It reproduced the same symptom observed before merging PR #96. A successful
retry does not establish that this issue is resolved.

## Observations on 2026-09-28

The test deliberately drops two adjacent DATA packets. Row FEC cannot repair
both, so Robotweax must request retransmission. In failing cases the receiver
obtains 194,768 of 197,400 bytes: exactly two 1,316-byte payloads are missing.
The receiver reports one NAK and no decryption failures; the reference sender
reports zero received NAKs and zero retransmissions.

A focused local macOS series against the existing Haivision 1.5.7 reference
binary failed 9 of 60 attempts. In all nine failures the relay successfully
submitted the NAK via UDP to the sender endpoint with the matching sender
socket ID. Each NAK requested precisely the two dropped sequence numbers.
This rules out missing NAK generation and incorrect destination socket IDs
in these observations; successful UDP submission does not prove delivery
to or processing by the reference library.

A separate temporary diagnostic experiment delayed NAK forwarding by 10 ms.
All 60 attempts passed. This is evidence of timing sensitivity, not proof
of a particular implementation defect. No delay is added to the committed
harness, and no production protocol behavior or pass criterion is changed.

## Reproduction

Build the normal static test peer and use the pinned reference peer:

```sh
python3 interop/run_fec_interop.py \
  --robotweax-peer build/robotweax_srt_interop_peer \
  --reference-peer reference-api-peer \
  --rendezvous-scenario aes256-row-fec-rendezvous-haivision-to-robotweax-burst \
  --repeat 60 > fec-nak-diagnostic.log 2>&1
```

Every attempt runs the full scenario assertions. Any failure makes the
command fail even if later attempts pass. Without the diagnostic selector,
the normal full FEC matrix and role-coverage requirements are unchanged.

Failure traces now include the NAK target socket ID, wire timestamp, size,
observation time, successful UDP forwarding time and endpoint, and the latest
forwarded handshake in each direction. `forwarded: false` distinguishes
observation from successful submission to UDP. Times are monotonic values
from the relay process and should only be compared within that run.

## Remaining investigation

Capture the same repeated scenario on the Linux runner and correlate the
relay's NAK submission with UDP reception at the reference endpoint. Check
handshake handoff and reference control-packet processing before assigning
blame to either implementation. Also verify why no second useful NAK is
observed before the 120 ms live delivery deadline. Add a deterministic
protocol regression test once the faulty transition is identified.
