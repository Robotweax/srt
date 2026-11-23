# Robotweax SRT 0.3.5 — Mobile Timer Pacing and Transport Reliability

Version 0.3.5 changes the default pacing policy for mobile builds and improves
short-interval pacing, receive recovery, and Windows process shutdown.
The [release process](release-process-0.3.5.md) describes the remaining
qualification and publication steps.

## Versions and upgrade

The project version is **0.3.5**, the shared-library ABI is **0.3**, and the
compatible SRT API and `srt_getversion()` report **1.5.7**. The public C ABI
retains all 86 exports from [0.3.4](release-notes-0.3.4.md). Applications
moving from ABI 0.2 still require rebuilding. Rebuild source-tree C++
consumers with matching headers and library because internal types including
`PacketPacer` changed layout.

## Mobile pacing

Android and iOS builds now default to timer waits for sub-millisecond pacing
deadlines, including when timer wakes are coarse. Set
`ROBOTWEAX_SRT_MOBILE_PACING=legacy` at configure time to restore the previous
policy. The mobile configure helper accepts `--mobile-pacing legacy`. The
former `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_MOBILE_PACING` CMake setting is
deprecated. The helper ignores the old `--experimental-mobile-pacing` flag.

For an Android ARM64 sender tested on a Samsung Galaxy A56 at 2 to 8 Mbit/s,
timer pacing roughly halved sender CPU time with the same or higher delivered
rate. Battery energy was outside that measurement. See the
[mobile build and measurement guide](mobile-building.md) for the test profiles
and the separate iOS results.

Experimental Live pacing coalescing can let one timer wake serve several
packet deadlines. Set
`ROBOTWEAX_SRT_MOBILE_PACING_COALESCE_MICROSECONDS` to a value from 0 to 10,000.
Zero is the default and disables coalescing. A positive value requires timer
pacing. The mobile pacing selection and coalescing settings apply to Android
and iOS builds.

## Timing and receive recovery

- Pacing carries fractional microseconds between packets on a continuous
  schedule. When a sequence restarts after a pause, the first interval still
  rounds up. The File rate controller also retains fractional precision when
  it lengthens the packet period on loss. Both changes matter most at
  multi-gigabit rates with packet intervals of a few microseconds.
- On Linux and macOS, recovery from a brief coarse-timer episode starts
  with a probe after 5 ms. After four punctual probes the channel returns to
  timer waits. Repeated late probes and closely spaced episodes increase the
  spacing up to 100 ms.
- When a newly detected one-packet gap would immediately trigger a NAK, the
  receive loop rechecks it on the next nonblocking UDP read in the same
  processing slice. Data already queued can fill the gap before the receiver
  reports it. If the gap persists or the slice ends, the receiver reports it
  using the normal loss path.

## Socket and platform corrections

- Set `SRTO_PACKETFILTER` or `SRTO_CONGESTION` to an empty string to reset the
  corresponding option. Existing rules about when options may be set still
  apply. See [socket options](socket-options.md).
- Invalid single-stream connection-group options are rejected before endpoint
  sockets are opened. The group interoperability harness records listener
  output on readiness failure and retries the specific Haivision 1.5.7 caller
  race once.
- Windows DLL builds avoid worker teardown during process termination.
  Applications still use normal `srt_close()` and `srt_cleanup()` to shut down
  explicitly during operation.

## Support boundaries

Null packet deletion, pinned-key peer authentication, UDP batching, ACP1 and
hybrid control retain their documented experimental status. The current
[compatibility](compatibility.md) and [limitations](limitations.md) guides
describe supported modes and boundaries.
