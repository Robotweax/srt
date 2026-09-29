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

## Linux reproduction and receiver correction

The isolated Linux baseline run
[36409639937](https://github.com/Robotweax/srt/actions/runs/36409639937)
failed 35 of 60 uninstrumented attempts. A separate 20-attempt run under
`strace` passed completely and showed NAK reception via `recvmsg`. Instrumentation
therefore masks the failing timing; those successful syscall traces do not
prove reception of the NAKs from the failed baseline attempts.

A concrete receiver-side defect was reproduced independently of the reference:
the initial RTT estimate schedules the first repeated NAK approximately 150 ms
after the loss report. A subsequent ACKACK can establish a 1 ms RTT, but the
pending NAK deadline was not advanced. With 120 ms live latency, the next useful
request could therefore arrive after the missing packets' delivery deadline.

The correction refreshes the loss timer on a valid ACKACK RTT sample and brings
an outstanding NAK deadline forward when its interval shrinks. It retains the
20 ms minimum interval (libsrt parity) and never postpones an already earlier deadline. There
is no artificial delay, retry-to-pass logic, or change to FEC recovery criteria.
Two deterministic native regression tests fail before this change and pass
after it, covering the timer directly and FEC `arq:onreq` with no subsequent DATA.

Validation of the correction: 739/739 native tests, the full local FEC matrix,
and 60/60 uninstrumented local attempts of the originally failing scenario
passed. The matching Linux run
[36410079875](https://github.com/Robotweax/srt/actions/runs/36410079875)
passed 60/60 uninstrumented and 20/20 separately traced attempts, compared with
35/60 failures in the earlier Linux baseline.

A temporary local control deliberately discarded the first NAK: 10/10 transfers
still passed. Three further instrumented controls explicitly observed the
replacement NAK about 71–77 ms after the discarded one and complete successful
recovery. The printed `arq_naks=1` is the reference sender's received-NAK statistic,
not the total number emitted by Robotweax or observed by the relay. These
controls are supplemental evidence; the committed deterministic session test
covers the lost-first-NAK and valid-ACKACK transition without wall-clock races.

The exact reason the reference sometimes does not process the first NAK remains
unproven. The correction addresses Robotweax's demonstrated failure to reschedule
recovery after learning the RTT; it does not claim to fix a Haivision internal
handoff race.

The Linux comparison can be dispatched independently of normal CI or release
integrations with `timing-diagnostics.yml`, investigation `fec-nak`. It preserves
60 baseline attempts and 20 separately instrumented attempts as an artifact,
including failures. No upstream source inspection is needed.
