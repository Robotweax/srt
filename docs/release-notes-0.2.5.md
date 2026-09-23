# Robotweax SRT 0.2.5 — Ecosystem Support

Status: **release candidate; publication and final artifact acceptance pending**.

Project version: **0.2.5**. Shared-library ABI line: **0.2**.
Compatible SRT API and `srt_getversion()`: **1.5.7**.
OpenSSL remains the default backend; BCrypt remains experimental.
AES-GCM remains an explicit default-off extension.

## Ecosystem integration

This release provides qualified, version-pinned source-build profiles for
GStreamer, VLC and OBS. The canonical
[ecosystem support matrix](compatibility.md#ecosystem-build-profiles) defines
platforms, roles, encryption, patches and exclusions. These are not universal
binary replacements for existing installations or complete desktop distributions.

- GStreamer: existing `srtsrc`/`srtsink`, media integrity, encrypted and cleartext
  Caller/Listener interoperability, controlled shutdown and listener reconnect.
- VLC: Linux SRT input/output with decoded video and outgoing media checks;
  a hash-guarded payload-option adjustment is required for the pinned revision.
- OBS: Linux, Windows x64 and Apple Silicon module and Qt desktop profiles;
  native output and FFmpeg media input select one Robotweax provider per process.
  Desktop profiles include encrypted duplex and automatic reconnect checks.
- OBS desktop builds apply a pinned, hash-guarded MPEG-TS lifecycle correction.
  The Windows preview ZIP is optional and manually invoked; CI does not create
  or publish an OBS binary distribution. No OBS package is a 0.2.5 SDK asset.

## Runtime and packaging changes since 0.2.4

- Delayed NAKs no longer cause DROPREQ for cumulatively acknowledged sources
  still waiting in a receiver's TSBPD buffer. Abandoned unacknowledged sources
  retain drop-request retries.
- UDP buffer selection reads back actual capacity and uses bounded fallback
  for capacity errors. Invalid socket requests remain errors.
- Lite-ACK handling preserves receive-window credit; empty receive buffers
  avoid unnecessary readiness scans. The unqualified paced-poll skip was removed.
- Optional Linux sender-drop diagnostics distinguish local retransmission
  abandonment from receiver delivery loss, with causal and provenance checks.
- The UDP/SRT bridge reserves its input port during connection setup and drops
  reservation backlog before forwarding live input.
- Windows SDK packaging supports Azure Artifact Signing and signature rejection
  tests. v0.2.4 assets are not changed or re-signed.

See the [changelog](../CHANGELOG.md) for the complete change summary.

## Compatibility and upgrading

The public C export inventory is unchanged from 0.2.4; the ABI line stays 0.2.
The SRT API target remains 1.5.7 independently of the Robotweax version.
Rebuild direct C++ consumers: private state was added to the source-tree C++
`ReliabilitySession` class after 0.2.4, and matching exported C symbols do not
establish C++ class-layout compatibility. Build third-party integrations against
the selected Robotweax headers and library and retain their provider checks.

Windows SDKs remain separate OpenSSL and BCrypt static-library installers for
Debug/Release and Win32/x64/ARM64. The Windows OBS profile instead builds its own
shared BCrypt compatibility DLL; it does not qualify those SDKs inside OBS.
Installer installation/signing details are in the
[Windows SDK guide](https://github.com/Robotweax/srt/blob/main/packaging/windows/README.md).

## Qualification limits and release acceptance

The pre-release integration baseline
[`8da678f2029b1b7636a12fc424fea5bdfdfc9db4`](https://github.com/Robotweax/srt/tree/8da678f2029b1b7636a12fc424fea5bdfdfc9db4)
passed all seven ecosystem CI jobs in
[run 35872509479](https://github.com/Robotweax/srt/actions/runs/35872509479).
This is evidence for that baseline, not acceptance of a later 0.2.5 tag.
Linux OBS desktop is included in the Linux OBS integration job. The observed
Linux desktop soak was approximately 38 seconds, not a long-duration test.

Core protocol qualification does not automatically qualify corresponding
GStreamer/VLC/OBS options. Physical capture devices, hardware encoders, adverse
networks, quantitative A/V synchronization and long-duration desktop operation
remain outside these profiles. Independent cryptographic review remains
outstanding. No new throughput or latency improvement is promised.

Before publication, record the final release commit, successful required CI run,
tag-triggered SDK build/test run, signature verification for both installers,
installed-consumer checks and final asset hashes. Review the resulting draft
release before publishing; signing requires the release environment's human
approval. The version tag must exactly match CMake. Never overwrite old tags or
assets. Third-party application binaries are not part of this release.
