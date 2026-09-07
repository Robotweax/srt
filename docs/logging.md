# Logging compatibility

Robotweax SRT implements the Haivision SRT v1.5.7 logging-control surface
without adding logging dependencies or policy to the native protocol core.
The public boundary includes:

- `srt_setloglevel` with the conventional syslog severity values;
- `srt_addlogfa`, `srt_dellogfa`, and `srt_resetlogfa` over the fixed public
  functional-area range 0 through 63;
- `srt_setlogflags` with the four compatible time, thread, severity, and
  end-of-line suppression flags;
- `srt_setloghandler` and the exact `SRT_LOG_HANDLER_FN` callback signature;
- the v1.5.7 `SRT_LOGFA_*`, level, and flag constants in the public headers.

## Architecture boundary

Logging is owned by `src/compat/logging.cpp`. The transport state machines,
packet codecs, reliability, congestion control, crypto, FEC, and TSBPD code do
not call it. Selected public lifecycle operations emit compatibility events at
the API boundary. This keeps the data path free of formatting, allocation,
callback, and output-policy costs and leaves room for a separate production
telemetry interface later.

The configuration uses a fixed 64-bit functional-area set and does not grow
with input. `srt_resetlogfa` accepts at most 64 entries, ignores out-of-range
values, and safely handles a null pointer. These are deliberate safety
improvements over relying on unchecked bitset positions or pointer arithmetic.

## Callback and concurrency contract

Handler and opaque-context replacement is serialized with delivery. Once a
callback begins, a concurrent replacement waits until that invocation has
finished, so one event cannot combine an old handler with a new context.
Handler-side configuration changes are supported. Recursive log emission on
the same thread is suppressed, and a C++ exception raised by a handler is
caught before it can cross the C ABI or alter the result of the triggering SRT
operation.

When no handler is installed, enabled events are written to `stderr`. The
default threshold is `LOG_WARNING`, matching v1.5.7; the currently emitted
compatibility events are `LOG_NOTICE`, so normal applications receive no new
output unless they explicitly increase the logging level.

## Formatting

The callback receives the compatible level, source file, source line, area,
and formatted message fields. The four `SRT_LOGF_DISABLE_*` flags independently
control time, thread identity, severity/logger prefix, and the final newline.
Tests disable all optional fields to enforce a deterministic exact string in
addition to checking individual metadata fields.

The default timestamp and thread rendering are intentionally Robotweax-owned;
applications that require structured, stable metadata should consume the
separate callback arguments instead of parsing a reference-library text
prefix.
