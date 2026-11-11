# Package inputs and qualification — 0.3.3

The initial product failed full CI. Corrections now have local regression evidence;
new frozen-source hosted qualification is pending. This record does not authorize publication.
Project version is 0.3.3, ABI is 0.3, compatible API is 1.5.7; all 85 C exports
from 0.3.2 are preserved. Historical qualification records remain unchanged.

## Current Ubuntu 26 product

The owner requested migrating all current Linux CI and package qualification to
Ubuntu 26.04 on 11 November 2026, superseding the earlier Ubuntu 24 baseline.
All active runner labels, the compiler container, package containers, current
platform contracts and current integration instructions select Ubuntu 26.04.
The standalone preview workflow was folded into the primary matrices. Historical
results below retain their real environments and are not Ubuntu 26 acceptance.

- Product commit: `57cf22319ead3fd74bf7f9b6f2dfe551938639b6`.
- [Public archive](https://github.com/Robotweax/srt/archive/57cf22319ead3fd74bf7f9b6f2dfe551938639b6.tar.gz); 3160738 bytes.
- SHA-256: `2ae9e816c64fa10d3caa21f358e608f889637a98754225da3e656d948096dc55`.
- SHA-512: `a88db33db883cfad7963a86cccd9a3c69f3c1070de1564057bb2797abe72073c743b55cfe15e6d5ed944a0f0caa25f664d35f04bd7b8dc5df4443c342ce63a3e`.
- All 952 regular files and executable bits verified against Git.
- [Current identity](source-0.3.3.json).

Local validation: 874 Python tests, 100-document consistency check, Ubuntu 26.04
container package-index refresh and availability of GCC 15/CMake/debhelper.
Native library sources and public headers are unchanged. Full hosted Ubuntu 26
CI, ecosystem, providers, hybrid/sanitizers and package qualification are pending.
The mirror-DNS and encrypted FileCC recovery failures remain recorded; changing
the OS baseline does not dispose of those failures or waive their gates.

## Superseded Ubuntu 24 product and accepted timeout disposition

- Product commit: `d6ad0c3e595b6ce5d518d3310e103e480fbc633d`.
- [Public archive](https://github.com/Robotweax/srt/archive/d6ad0c3e595b6ce5d518d3310e103e480fbc633d.tar.gz); 3160300 bytes.
- SHA-256: `4798208d99d66417209a461d4b24e4a6269b6ea740fbee8d8beab7e5b295258d`.
- SHA-512: `b0d1dc75aafc4d5475e371025d3488060d369648e49851438374cde47d81dd73a0eda38b5c5b766ac21b2a0fe0f673df547dfbd98d5b9e0ce232b00da5416675`.
- All 952 regular files and executable bits verified against Git.
- [Source identity](source-0.3.3.json); [accepted risk](acceptance-0.3.3.json).

On 11 November 2026 the owner accepted proceeding with final qualification while
retaining the original macOS identity-rendezvous timeout as an unresolved risk.
This disposition is not a root-cause fix or blanket acceptance of other failures.
The old harness deleted peer logs. Failures now emit both peer logs, exit states
and received-byte counts before cleanup, and preserve the original exception.
The diagnostic regression test and all 873 Python tests passed.

After 600 isolated rendezvous successes across local Release/Debug and hosted
Debug, the full original sequence passed 50 times locally and 50 times in hosted
macOS Debug: 900 cases each, all 18 policy/role combinations and payload integrity
verified in every sequence, without retries or increased deadlines.
[Hosted full-sequence evidence](https://github.com/Robotweax/srt/actions/runs/43035645485).
The original timeout remains unexplained. Keep the diagnostics and original
failure evidence available for recurrence.

The earlier GCC 15, Ubuntu DEB and hybrid-sanitizer failures during the latest
qualification attempt were package-mirror DNS failures before compilation/tests.
Original runs 43035645583, 43035645566 and 43035645536 remain historical. New
final-source qualification must pass; this disposition does not waive those gates.

## Superseded corrected product

- Commit: `7345251711709efc72e26be5af0deddc3038f970`.
- [Public archive](https://github.com/Robotweax/srt/archive/7345251711709efc72e26be5af0deddc3038f970.tar.gz); 3158793 bytes.
- SHA-256: `e36892f9607eee7ba4d525a9a4f8e5ed7f4e6357e37684b4976f35a49c6253e4`.
- SHA-512: `2d2fbc4cd61d169b023f4735ff553b229868092073d6b72e37a587199be2275ab3f801a356f7890592af4f066070a75a46eb773b6e97d9ae8671c8102d81141a`.
- All 950 regular files and executable bits verified against Git.
- [Current source identity](source-0.3.3.json).

Linux release jobs now select Ubuntu 24.04. The separate Ubuntu 26 workflow
runs default/hybrid native suites, FileCC and VLC without replacing release gates.
VLC's oracle is a synchronous libmpeg2 decoder required to produce all 50 distinct
fixture frames, independent of incomplete playback. Local VLC and network-frame
checks remain. Robotweax-sender rendezvous NAK cases now explicitly inject a
bounded loss window until the actual matching receiver NAK is forwarded; retries
are recorded, not silently accepted, and missing feedback fails closed.

Local corrections passed 872 Python tests, six real FFmpeg-to-VLC cleartext/CTR
runs, the FileCC baseline/IPv6/rendezvous-resilience matrix, and delayed-feedback
probes with two premature retries suppressed before matching NAK delivery. The
new regression tests reject incomplete/duplicate reference output, unrelated NAKs,
and missing matching feedback. Runtime library sources and exports are unchanged.
All original failures and the initial source record below remain historical;
these local results do not replace new hosted acceptance.

## Superseded initial product

- Product commit: `d1f1b15393c0d0b5598c0af308b24966cbbfbd5e`.
- [Public archive](https://github.com/Robotweax/srt/archive/d1f1b15393c0d0b5598c0af308b24966cbbfbd5e.tar.gz).
- Bytes: 3150219.
- SHA-256: `0bf14b9b0de8f8a0a5688dfd1ce6e5e4cf8bdb7cb0f8abd466e1d895cb6d6a99`.
- SHA-512: `7beb067c9e00be98c9e2722d83a7607a1960c1d88faaf7e992e724857c7823f2497148bc545a8e32288f8a3d01f9d413846d21f9b89df53a03f794e03175fed2`.
- All 944 regular files and executable bits matched Git.
- [Machine-readable identity and frozen paths](source-0.3.3.json).

Recipe/evidence commits must preserve the frozen paths. Product changes require
another archive and qualification. See the [release contract](../docs/release-process-0.3.3.md).

## Preparation evidence

The Python harness passed 868 tests. Changed C/C++ formatting passed with
clang-format 22.1.8. The versioned local macOS hybrid Release build passed all 183 CTest entries;
the default shared-library build passed all 20 entries. Isolated installation
validated all 100 public Markdown files. The 30-second-per-profile CTR/GCM
soak passed with commit binding to recipe head `98985fe11f4070b5a4617be27491974808f87699`;
all frozen product paths match the product commit. These local checks alone
do not establish release acceptance.

Previous signed Windows upgrade inputs are 0.3.2, with SHA-256:

- OpenSSL: `ad1998a27cd6e6136111ab024eb450cc666bc7911f2284fd9aae4ed1fbb5aff5`.
- BCrypt: `5b1af8638c0bc04622a9cd185e4e10ebf9739f977d4e375f26f15771497f0fa9`.

## Pending gates

Full source CI and ecosystem; all provider, multicast, ACP1, hybrid and sanitizer
matrices; Windows timer OFF; installed docs/consumers; commit-bound hybrid soak;
Homebrew, six vcpkg profiles, DEB/RPM; exact-source Linux/downstream qualification;
protected annotated tag, independent Windows rebuild/signing and final assets.

## Hosted results and first failures

- [Linux packages, 43010984962](https://github.com/Robotweax/srt/actions/runs/43010984962): Ubuntu 24.04 and Fedora 44 passed.
- [Package managers, 43010984970](https://github.com/Robotweax/srt/actions/runs/43010984970): Homebrew and all six vcpkg profiles passed.
- [BCrypt, 43010984969](https://github.com/Robotweax/srt/actions/runs/43010984969): passed.
- [Hybrid, 43010984967](https://github.com/Robotweax/srt/actions/runs/43010984967): both sanitizer jobs and Windows timer-OFF passed; remaining jobs pending at this update.
- [Full CI, 43011018905](https://github.com/Robotweax/srt/actions/runs/43011018905): failed FileCC and VLC jobs; aggregate reference-interoperability failure follows FileCC. Other jobs remain in progress.

### FileCC recovery evidence

[File-base job 130492425521](https://github.com/Robotweax/srt/actions/runs/43011018905/job/130492425521)
failed `filecc-ipv6-rendezvous-nak-drop-robotweax-to-haivision` because no matching
NAK preceded the retransmission. Delivery was byte-identical (1,491,323 bytes).
The original drop was sequence 1174101912, occurrence 70, relay ordinal 143;
its first retransmission was ordinal 146, 6,798 microseconds later. No later
DATA had exposed that sequence gap before the retry. The only observed NAK
requested earlier sequence 1174101843. This does not prove the required NAK
recovery, and does not yet establish whether transport scheduling or fixture
assumptions need correction. Do not relax the assertion or treat a rerun as a fix.

### VLC reference validity

[VLC job 130490478246](https://github.com/Robotweax/srt/actions/runs/43011018905/job/130490478246)
failed the first cleartext FFmpeg-to-VLC decoded-frame identity check. The
`vlc-evidence` artifact retains original frames and logs. The local reference
contains 46 hashes, 34 unique, with libmpeg2 invalid-picture and buffer-deadlock
errors. The network case contains 20 distinct hashes, only eight present in the
reference. Investigate reference decoding and the receive path; neither transport
corruption nor a harmless fixture failure is established. Keep exact identity checks.

### Downstream request provenance

The first smoke/interop/telemetry requests (43010990408, 43011020124,
43011022604) failed before traffic: the Lab rejected the exact product SHA as
not directly advertised by a branch or tag. The recipe branch had advanced.
`dev/release-0.3.3-product` now advertises the unchanged product commit. The
corrected requests retain the same exact SHA and companion inputs:

- [Smoke 43011513334](https://github.com/Robotweax/srt_network_lab/actions/runs/43011513334).
- [Interop 43011518069](https://github.com/Robotweax/srt_network_lab/actions/runs/43011518069).
- [Telemetry 43011522479](https://github.com/Robotweax/srt_network_lab/actions/runs/43011522479).

Inputs: Haivision `899348d8318eb9a3c5a5b6ec43c4a1114288773a`,
Telemetry `7a6b8eff65c686ccafa36a0adace148a8b87d3ca`,
Doctor `88f8e33851f4fa975ab1b708e9f707d27505a399`; telemetry endpoint profile.
These requests are pending, not passing downstream qualification evidence.

No release tag or published release has been created. Any product or harness
correction inside the frozen paths requires a new source freeze and qualification.

## Follow-up investigation

[Machine-readable comparison and reproduction record](investigation-0.3.3.json).
The failing VLC network case's 20 frame hashes match the successful 0.3.2
network case exactly, in order. All occur in the successful reference; the
November 9 reference also matches 0.3.2. The failing local reference replaces
positions 1–12 (zero-based) with the valid frame from position 13. It has
46 captures but only 34 distinct hashes, compared with 46/46 previously.
The local reference checker accepts ten distinct hashes, so it accepts this
incomplete reference and subsequently attributes the mismatch to network decoding.
This locates the immediate failure; the underlying decoder/scheduling trigger
remains unresolved. The VLC harness, pinned application revisions and libmpeg2
package are unchanged; the hosted runner image changed. No runner-image
causality is established.

Forty local Linux/ARM reference decodes with pinned VLC/FFmpeg (20 ordinary,
20 under CPU contention) did not reproduce invalid-picture errors. Fifteen
FileCC repetitions passed across both rendezvous roles. These are diagnostic
results, not replacements for the failed hosted x86-64 evidence.
The FileCC harness and flight-wide RTO timer are unchanged from 0.3.2. More
packets in an application write do not guarantee their transmission before an
existing flight timeout. Preserve the NAK gate; instrument sender timeout/ACK
selection before deciding how to enforce its gap-exposure precondition.
Neither issue is marked fixed, and release acceptance remains blocked.
