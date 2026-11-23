# Security-sensitive changes for Robotweax SRT 0.3.5

This pre-freeze review covers the transport and lifecycle changes from
`v0.3.4` through upstream commit `31475429875f0604c83fe60676f7653876742579`.
It records the scope and remaining evidence for the
[0.3.5 release process](release-process-0.3.5.md). The
[0.3.4 delta review](security-delta-0.3.4.md) and
[November 4, 2026 source review](security-review-2026-11-04.md) cover their
recorded revisions.

## Changes reviewed

- **Receive feedback.** Same-pass gap handling keeps one pending recheck per
  datagram channel. The next read must match the missing sequence, destination
  socket and peer even to be considered for recovery, and the packet still
  goes through normal admission and authentication. A nonmatching read,
  empty read or end of slice flushes the pending feedback. The deterministic
  runtime test covers both suppression on successful repair and feedback
  when repair is absent.
- **Group admission and socket options.** Single-stream option validation is
  performed before opening group endpoints. Empty-string resets use the
  existing option validation and restrictions. The listener rollback cleanup
  removes a branch premised on group handle reuse. The group registry allocates
  new handles for new groups. Keep the option validation, lifecycle and mixed
  implementation group cases in the release matrix.
- **Windows process termination.** Shared Windows builds distinguish
  process exit from explicit DLL unloading. Windows-specific coverage is
  needed for the process-exit probe and installer lifecycle. Source review
  and a non-Windows build cannot verify loader behavior.
- **Experimental receive and peer authentication coverage.** The release
  delta adds tests that observe the existing batch length clamp and peer
  storage clearing, and probe expected-tag erasure. These tests were
  identified as missing from the 0.3.4 product. The peer-authentication
  protocol remains experimental and default off.
- **Pacing.** The mobile policy and opt-in coalescing change scheduling.
  Coalescing is bounded and applies to Live pacing for new data. The user
  can select the legacy mobile policy at build time. Coverage for physical
  devices and the changed timing paths is part of frozen-source qualification.

## Open dispositions

- Peer-authentication listener admission remains a documented limitation.
  The risk decision for 0.3.4 is historical. Record an owner decision for
  0.3.5 and the selected source. [Issue 406](https://github.com/Robotweax/srt/issues/406).
- The macOS identity Rendezvous timeout and intermittent OBS desktop
  reconnect failures remain open for release disposition. The deadline
  regression tests and the test allowance added since 0.3.4 do not resolve
  the recorded identity case. Preserve results for the native macOS and OBS
  desktop gates.
- The group interoperability harness retries the identified Haivision 1.5.7
  caller race once and adds listener output for readiness failures. Keep that
  reference race distinct from unexplained hosted failures and retain first
  failures in qualification. [Race disposition](https://github.com/Robotweax/srt/issues/417),
  [hosted failure history](https://github.com/Robotweax/srt/issues/407).

The release owner has yet to record any 0.3.5 risk acceptance. The review
status applies to this candidate delta and will need to be checked against
any source changes before freeze.
