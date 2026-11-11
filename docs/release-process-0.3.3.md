# Release contract for Robotweax SRT 0.3.3

The [release notes](release-notes-0.3.3.md) define the product scope. Project
version is 0.3.3, ABI remains 0.3 and compatible SRT API remains 1.5.7.
Multicast, ACP1 and hybrid control retain their documented experimental limits;
this release does not promote them to general production or security support.

## Product and documentation freeze

Finish version headers, the 85-symbol ABI snapshot, release notes, changelog,
option guides, samples and Windows SDK documentation before source freeze.
Validate the installed documentation from an empty prefix. Review the source
delta after the recorded 2026-11-04 security review, retaining findings and their
disposition; do not relabel that historical scan as a scan of this release.

Verify hybrid soak build provenance against the configured source checkout and
refuse foreign build directories or source changes during the build. Reconcile
the historical hybrid review packet with current source and evidence, preserving
accepted experimental availability, timing, source-failure and deployment limits.

Publish one immutable product commit, verify archive bytes and executable bits
against Git, and record SHA-256/SHA-512. Homebrew, vcpkg, DEB/RPM and exact-version
consumers must select this product. Later recipe/evidence commits must preserve
the frozen product and installed documentation. Product changes require a new
freeze and qualification; failures and superseded records remain historical.

## Exact-source qualification

Pin Linux release qualification to `ubuntu-26.04`, including package builds and
compiler-specific containers. A versioned label still receives image updates,
so retain the actual image identity in run evidence. Native default/hybrid,
FileCC, VLC and other ecosystem checks run in the primary Ubuntu 26 matrices.

Require full source CI and its Required CI gate, including the complete ecosystem
suite. A path-selected PR run that omits OBS, VLC, FFmpeg or GStreamer does not
satisfy this gate. Do not duplicate the ecosystem suite with release-integrations
when full CI already runs it on the same source.

Require OpenSSL/BCrypt/Mbed TLS, ACP1, hybrid Release and GCC/Clang sanitizer
matrices, ABI/export checks, package consumers, Homebrew source installation and
audit, six vcpkg profiles and Linux packages. Require the five non-skipping
multicast OS/backend rows, ARQ-first cases and the hybrid sample's cleartext,
CTR/GCM, buffered catch-up, credential and option-rejection cases.

Record hosted macOS's exclusion of native hybrid/network gates separately from
local macOS execution; do not present those exclusions as runtime passes.
Qualify the Windows timer-period OFF build separately from the default ON profile.
Run commit-bound hybrid soak qualification with its declared rate, retention,
queue and recovery bounds; physical-network/scale claims remain out of scope.

Repeat issue #360's Linux native and full Telemetry/Doctor downstream checks on
the frozen product, retaining companion identities, original outcomes and cleanup
evidence. Prepared scripts and earlier revisions are not exact-source results.
Preserve first failures; a green rerun alone does not establish their cause.

## Windows upgrade and protected rebuild

Qualify upgrades, repair, coexistence and installed consumers from the immutable
signed 0.3.2 OpenSSL and BCrypt SDKs. Downloaded inputs must match their recorded
SHA-256 values and have valid Authenticode signatures.

After frozen-source acceptance, create the protected annotated `v0.3.3` tag.
Its Windows SDK workflow performs one fresh independent rebuild in the protected
environment without ordinary producer artifacts, producer caches or OIDC signing
authority. The signer verifies provenance, signs and retests the exact immutable
artifact bytes. Existing protected-environment approvals remain required.
Do not duplicate tag signing with pre-tag signing or the optional unsigned
`qualify_rebuild` diagnostic. Preserve failed rebuild evidence.

## Publication

Keep the release a draft until final-tag CI, packages and protected signing pass.
Verify the live annotated tag object/commit, download the exact signed assets,
check their digests and provenance, and publish only the qualified bytes.
Record run identities in release evidence without modifying frozen installed
documentation. Never move a published tag. Preserve 0.3.2 artifacts; bottles,
PPA/COPR and other distribution repositories have separate publication checks.
