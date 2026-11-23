# Package inputs and qualification for Robotweax SRT 0.3.5

On 23 November 2026 the release owner approved the 0.3.5 scope and directed the
freeze of product commit `3415d7c155d393755c8e49f88db4e68789fdf1b6`.
The project version is 0.3.5, shared-library ABI is 0.3, and the compatible SRT
API is 1.5.7. The public C export snapshot contains the same 86 entries as 0.3.4.
The [acceptance record](acceptance-0.3.5.json) captures the three residual risks.

## Frozen source

The product ref is `dev/release-0.3.5-product`. Two downloads of the GitHub
commit archive were byte-identical. All 1,005 regular files and executable bits
matched Git and the archive contained no extra files. The archive is 3,378,654
bytes. The [source manifest](source-0.3.5.json) records both digests and the
frozen paths.

The [release process](../docs/release-process-0.3.5.md) defines the required
evidence and the protected rebuild on the final tag.

## Hosted qualification started

The eight source and package workflows were dispatched on
`dev/release-0.3.5` at recipe commit
`3c679f97bcaa869be82ef4ac5e8138dcf8338c36`. A Git diff against the product
confirmed all frozen paths match. Package jobs select the commit archive
above. The full source CI and Windows SDK workflows used default inputs.

| Workflow | Run |
| --- | --- |
| Full CI, including the ecosystem suite | [45240777216](https://github.com/Robotweax/srt/actions/runs/45240777216) |
| Ubuntu DEB and Fedora RPM | [45240776987](https://github.com/Robotweax/srt/actions/runs/45240776987) |
| Homebrew and vcpkg | [45240777250](https://github.com/Robotweax/srt/actions/runs/45240777250) |
| Unsigned Windows SDK and 0.3.4 upgrades | [45240777154](https://github.com/Robotweax/srt/actions/runs/45240777154) |
| BCrypt | [45240780441](https://github.com/Robotweax/srt/actions/runs/45240780441) |
| ACP1 | [45240777182](https://github.com/Robotweax/srt/actions/runs/45240777182) |
| Hybrid control, including Linux soak and sanitizers | [45240777139](https://github.com/Robotweax/srt/actions/runs/45240777139) |
| Peer authentication | [45240777141](https://github.com/Robotweax/srt/actions/runs/45240777141) |

Downstream Lab requests explicitly select the frozen product. They reuse the
0.3.4 Haivision, Telemetry and Doctor pins for comparison. The endpoint profile
is `telemetry`.

| Profile | Run |
| --- | --- |
| Smoke | [45240806073](https://github.com/Robotweax/srt_network_lab/actions/runs/45240806073) |
| Interop | [45240808065](https://github.com/Robotweax/srt_network_lab/actions/runs/45240808065) |
| Telemetry and Doctor | [45240805710](https://github.com/Robotweax/srt_network_lab/actions/runs/45240805710) |

Run links record dispatch. Successful completion and any first failures will
be recorded after reviewing the jobs and their artifacts.

## Native macOS

A clean checkout of the product commit passed all 22 CTest cases labelled
both `hybrid` and `network` on macOS 27.2.1 ARM64, Release, OpenSSL 3.6.5,
with AEAD, ACP1 and hybrid control enabled. The [strict soak record](soak-0.3.5-macos.json)
also passed 30 seconds each of CTR and GCM. Each profile sent 15,000 loaded
messages to two receivers at 499 packets per second on IPv4 loopback.

## Linux packages

Both jobs passed on attempt 1 of [45240776987](https://github.com/Robotweax/srt/actions/runs/45240776987),
including the archive integrity, package installation, consumer, coexistence
and removal checks.

- Ubuntu 26.04 DEB image `ubuntu@sha256:d66b366dac61b575bee2aef07e9c26e8070096fbe84a3d4d3eb0da069b5d76db`.
  [Job 137924508270](https://github.com/Robotweax/srt/actions/runs/45240776987/job/137924508270).
- Fedora 44 RPM image `fedora@sha256:15da97ed9aa94dd3d92c3925eb89520f6c80e07ea7ee24cba848d1f8b6912b3e`.
  [Job 137924508772](https://github.com/Robotweax/srt/actions/runs/45240776987/job/137924508772).
