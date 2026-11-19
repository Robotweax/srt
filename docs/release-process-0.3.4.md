# Release contract for Robotweax SRT 0.3.4

The [release notes](release-notes-0.3.4.md) define the product scope. Project
version is 0.3.4, ABI remains 0.3 and compatible SRT API remains 1.5.7.
Multicast, ACP1, hybrid control, null packet deletion, peer authentication and
UDP batching retain their documented experimental limits; this release does not
promote them to general production or security support.

## Product and documentation freeze

Finish version headers, the 86-symbol ABI snapshot, release notes, changelog,
option guides, samples and Windows SDK documentation before source freeze. The
snapshot adds only `robotweax_srt_epoll_wake` to the 0.3.3 exports. Validate the
installed documentation from an empty prefix. Review the source delta after
`v0.3.3`, retaining findings and their disposition; do not relabel the
2026-11-04 security review or the 0.3.3 delta as a scan of this release.

Publish one immutable product commit, verify archive bytes and executable bits
against Git, and record SHA-256/SHA-512. Homebrew, vcpkg, DEB/RPM and exact-version
consumers must select this product. Later recipe/evidence commits must preserve
the frozen product and installed documentation. Product changes require a new
freeze and qualification; failures and superseded records remain historical.

## Exact-source qualification

Pin Linux release qualification to `ubuntu-26.04`, including package builds and
compiler-specific containers, and retain the actual image identity in run
evidence.

Require full source CI and its Required CI gate, including the complete ecosystem
suite. A path-selected PR run that omits OBS, VLC, FFmpeg or GStreamer does not
satisfy this gate; dispatch full CI on the release branch after every push. Do
not duplicate the ecosystem suite with release-integrations when full CI already
runs it on the same source.

Require OpenSSL/BCrypt/Mbed TLS, ACP1, hybrid Release, peer-authentication and
GCC/Clang sanitizer matrices, ABI/export checks, package consumers, Homebrew
source installation and audit, six vcpkg profiles and Linux packages. Require
the non-skipping multicast OS/backend rows, ARQ-first cases, the hybrid sample
cases and the null packet deletion interoperability cases with the current
identifiers.

Record hosted macOS's exclusion of native hybrid/network gates separately from
local macOS execution; do not present those exclusions as runtime passes.
Run commit-bound hybrid soak qualification with its declared bounds. Repeat the
downstream Telemetry/Doctor checks on the frozen product. Preserve first
failures; a green rerun alone does not establish their cause.

## Windows upgrade and the single protected rebuild

Qualify upgrades, repair, coexistence and installed consumers from the immutable
signed 0.3.3 OpenSSL and BCrypt SDKs. Downloaded inputs must match their recorded
SHA-256 values and have valid Authenticode signatures.

The from-source OpenSSL and SDK rebuild takes about 90 minutes of runner time
and runs exactly once for this release, in the protected environment on the
final tag:

- Before the tag, dispatch the Windows SDK workflow on the frozen commit with
  no inputs. This runs validation, the twelve-variant build matrix, both
  installers and coexistence, and skips both rebuild jobs. The rebuild compiles
  each variant with the same build script, so this covers its compile content.
- Do not dispatch `qualify_rebuild`, and do not dispatch `sign` on `main`. Each
  would be an additional rebuild.
- After frozen-source acceptance and the merge to `main`, create the protected
  annotated `v0.3.4` tag. Its Windows SDK workflow performs the one fresh
  independent rebuild without ordinary producer artifacts, producer caches or
  OIDC signing authority. The signer verifies provenance, signs and retests the
  exact immutable artifact bytes. Existing protected-environment approvals
  remain required.
- If signing or draft creation fails after a successful rebuild, re-run only
  the failed jobs. They consume the retained rebuilt artifact by its identity,
  which is kept for seven days. Never re-run all jobs of the tag run.
- A failure inside the rebuild job itself requires a second rebuild. Preserve
  the failed evidence and record the repetition and its cause.

## Publication

Keep the release a draft until final-tag CI, packages and protected signing pass.
Verify the live annotated tag object/commit, download the exact signed assets,
check their digests and provenance, and publish only the qualified bytes.
Record run identities in release evidence without modifying frozen installed
documentation. Never move a published tag. Preserve 0.3.3 artifacts; bottles,
PPA/COPR and other distribution repositories have separate publication checks.
