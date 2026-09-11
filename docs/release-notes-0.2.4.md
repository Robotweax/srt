# Robotweax SRT 0.2.4 release notes

Status: **release candidate; publication pending**.

Project version: **0.2.4**. Shared-library ABI line: **0.2**.
Default Haivision-compatible API profile and `srt_getversion()`: **SRT 1.5.7**.

This pre-1.0 release collects integration, packaging and runtime improvements
since 0.2.3. OpenSSL remains the source-build default on every platform.
AES-GCM remains an explicit, default-off extension. The public C export
baseline is unchanged from 0.2.3; rebuild native/private C++ consumers.

## Windows SDK distribution

Two installer variants are prepared, with explicit backend selection:

- `robotweax-srt-0.2.4-windows-sdk-openssl.exe`: static OpenSSL Crypto included.
- `robotweax-srt-0.2.4-windows-sdk-bcrypt.exe`: Windows CNG, no OpenSSL dependency
  bundled with the SDK.

Each contains public headers, static libraries for Debug/Release ×
Win32/x64/ARM64, licenses and MSBuild property sheets. Both SDKs can coexist in
separate directories. They set `ROBOTWEAX_SRT_OPENSSL` and
`ROBOTWEAX_SRT_BCRYPT`, respectively, not the old backend-neutral variable.
Import the selected root's `srt.props`. Legacy installer candidates must be
uninstalled first; same-backend upgrades require uninstall/reinstall.

The tag pipeline creates a draft with both installers and `SHA256SUMS` after
its checks pass. These filenames describe intended assets, not downloads
already published. Installers are unsigned; do not bypass organizational
security policy to install them. See the
[Windows SDK guide](https://github.com/Robotweax/srt/blob/main/packaging/windows/README.md).

The optional BCrypt backend includes provider differential, injected CNG failure
and encrypted interoperability coverage. CTR processing was optimized without
changing counter or error-clearing semantics. Measurements do not demonstrate
universal superiority over OpenSSL. See [BCrypt qualification](windows-bcrypt.md).

## Runtime and integration changes

- Add the [UDP/SRT bridge](udp-srt-bridge.md): MPEG-TS datagram forwarding,
  IPv4 multicast, Caller/Listener/Rendezvous in both media directions, live
  statistics and optional reconnect.
- Fix application-singleton cleanup ordering for sockets, groups and epoll.
- Preserve a usable native UDP buffer when BSD/macOS rejects a requested
  larger allocation, including acquired UDP sockets.
- Prevent repeated non-progress ACKs from starving Live tail recovery.
  Periodic-NAK timeout recovery uses bounded, paced tail probes rather than
  repeated full-flight retransmission; fallback modes retain their contracts.
- Install `srt/access_control.h` and compatible C++ logging level aliases.
- Provide Robotweax version identification separately from the compatible
  Haivision API version.
- Namespace installed files by default; integrators using legacy discovery
  should review [build/install options](building.md) and [integration](integration.md).
- Add experimental [mobile build support](mobile-building.md), with Android
  emulator and iOS simulator smoke evidence, not general physical-device qualification.

## Qualification limits and release acceptance

The limits in [Compatibility](compatibility.md), [Known limitations](limitations.md)
and the [AES-GCM contract](aes-gcm-contract.md) remain applicable. This release
does not establish arbitrary libsrt ABI substitution, hard-real-time behavior,
universal group failover guarantees or independently audited cryptography.
An independent cryptographic review remains outstanding. BCrypt and mobile
support retain their documented experimental status.

PR #29 and its merged Main commit passed CI before release preparation,
including twelve Windows SDK builds, both installers, both installation orders,
independent uninstall and installed x64 consumers. Those checks do not certify
the final 0.2.4 tag. ARM64 SDK packaging is build/link-tested on the x64 runner;
separate earlier ARM64 runtime evidence must not be conflated with final
installer acceptance.

Before publication: require green release-PR/Main CI, successful tag-triggered
installer builds and tests, and review both draft assets and checksums. Record
the publication date and final evidence at release acceptance. No previous tag
is moved or overwritten. Haivision reference binaries are not redistributed.
