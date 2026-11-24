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

## Hosted qualification

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

All eight release workflows and all three downstream Lab requests passed on
attempt 1. The Lab artifacts identify the frozen product and report PASS with
archive validation and cleanup verified for each request.

- Full CI passed 64 jobs, including the Required CI gate, all five multicast
  OS/backend jobs and the full ecosystem suite. The separate Live timing
  interoperability job was skipped by the full-suite condition. Linux Release
  ran on Ubuntu 26.04 image `20261116.202.1`.
- Package managers passed Homebrew source installation and audit plus all six
  vcpkg profiles. The Linux distribution builds are recorded below.
- Windows SDK passed 16 jobs, including all twelve build variants, both
  installers, and coexistence. The coexistence log confirms the signed 0.3.4
  upgrades, repair, downgrade rejection, both installation orders and
  independent uninstall. Both rebuild jobs, signing and draft creation were
  skipped as planned for this dispatch.
- BCrypt passed its default native and shared-library rows. Its standalone
  NPD and performance jobs correspond to separate workflow inputs.
- ACP1, peer authentication and all hybrid jobs passed. The Linux hybrid
  strict soak passed CTR and GCM, and GCC and Clang sanitizer jobs passed.
  The hosted macOS hybrid job excludes cases labelled both `hybrid` and
  `network`. Native results for those cases appear below.

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

## Merge, final tag and publication

The release owner approved using the physical Android and iOS pacing evidence
from [development PR 433](https://github.com/Robotweax/srt/pull/433).
[Release PR 435](https://github.com/Robotweax/srt/pull/435) records that decision,
DCO sign-off and early PR CI failures. The signed PR head passed the Required CI
gate on [attempt 3](https://github.com/Robotweax/srt/actions/runs/45252184176/attempts/3).
The identical tree also passed [full CI](https://github.com/Robotweax/srt/actions/runs/45246269538).
The PR merged as `fd9dac31675b12022579d28a748abb0839004cde`; its tree matches the
signed PR head, and its frozen paths and package inputs match qualification.
[Post-merge CI](https://github.com/Robotweax/srt/actions/runs/45262113042) passed.

The annotated tag `v0.3.5` has object ID
`d7b6ba7959593db1d37bac8178c6bff26ecfc5b0` and points to the merge commit.
[Final-tag CI](https://github.com/Robotweax/srt/actions/runs/45266706013) passed
on attempt 2. The [first attempt's OBS macOS desktop job](https://github.com/Robotweax/srt/actions/runs/45266706013/job/138015541188)
failed in debugger-capture verification. The retry passed and the cause of
the first failure remains unestablished. [Linux packages](https://github.com/Robotweax/srt/actions/runs/45266705607)
and [package managers](https://github.com/Robotweax/srt/actions/runs/45266705848)
passed on their first attempts.

The [tag Windows workflow](https://github.com/Robotweax/srt/actions/runs/45266705773)
passed on attempt 1. It performed one protected independent rebuild
(artifact `14868254541`) and signed and tested the resulting installers
(artifact `14869079365`, ZIP SHA-256
`3b83fb6b0849d9c088bb354091790a7c479f44a0d371c2a923893e359c302d8b`).
The [release-draft job](https://github.com/Robotweax/srt/actions/runs/45266705773/job/138071516775)
used Windows SignTool to verify the Robotweax GmbH Authenticode signatures and
trusted timestamps on both installers with zero warnings or errors.

The release owner approved publication. The [GitHub release](https://github.com/Robotweax/srt/releases/tag/v0.3.5)
was published on 23 November 2026 at 23:58 CET with the reviewed release notes.
Both public downloads are byte-identical to the verified signed artifact. The
published checksums match, and the published tag provenance matches the live
annotated tag and peeled release commit.

| Windows SDK | SHA-256 |
| --- | --- |
| OpenSSL | `fb82ee5ff1e301ca2811a84e2e313fd968c40788a1a06ed075c514e05a2eb7b0` |
| BCrypt | `6d67e0447c6386885f1afd046a15528b7560184687f9149a95079d5e09844644` |

## Homebrew bottle publication

The [0.3.5 bottle for Apple Silicon macOS 15](https://github.com/Robotweax/homebrew-tap/releases/tag/robotweax-srt-0.3.5)
(`arm64_sequoia`) was published on 24 November 2026.
[Native tap qualification](https://github.com/Robotweax/homebrew-tap/actions/runs/45340361072)
passed on attempt 1 at head `d7a81277139736aeca2c2bbb85da3d795912b656`.
Homebrew upgraded the checksum-verified published 0.3.4 bottle to the
candidate 0.3.5 bottle and ran the original C executable against the new keg
without rebuilding it. ABI 0.3, all 86 exports, installed C/C++ consumers,
109 public Markdown files, default-prefix relocation, coexistence and removal
passed.

The [tested artifact](https://github.com/Robotweax/homebrew-tap/actions/runs/45340361072/artifacts/14895972609)
and the independently downloaded public bottle match SHA-256
`33824f8702ec667745a5c4322f99dc71447ccc4da2ebfe15bce6a3e27a194c7f`.
The signed provenance for that digest verified against the tap's
[publication workflow and run](https://github.com/Robotweax/homebrew-tap/actions/runs/45341293566/attempts/1).
The tap's pull-request rule rejected the workflow's direct push. The same
formula and bottle metadata landed through [tap PR 16](https://github.com/Robotweax/homebrew-tap/pull/16)
as `0060f8445ba2e0ee3ff4a613bc06cef74a16d84f`.
