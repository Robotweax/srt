# Package inputs and qualification — 0.3.3

Release acceptance is blocked by full-CI failures. This record does not authorize publication.
Project version is 0.3.3, ABI is 0.3, compatible API is 1.5.7; all 85 C exports
from 0.3.2 are preserved. Historical qualification records remain unchanged.

## Immutable product

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
