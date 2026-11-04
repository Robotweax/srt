# Release contract for Robotweax SRT 0.3.2

Version 0.3.2 includes the fixes and opt-in profiles in the
[release notes](release-notes-0.3.2.md). ABI remains 0.3 and compatible SRT API
remains 1.5.7. ARQ-first is a regular opt-in option; multicast remains experimental
with an explicit trusted-group and physical-network qualification boundary.

## Freeze product and documentation

Finish version headers, the 85-symbol export snapshot, release notes, changelog,
option guides, examples and Windows SDK documentation before freezing source.
Install documentation into an empty prefix and validate links and release
identity. Record a public immutable product commit, verify archive file bytes and
executable bits against Git, and record SHA-256/SHA-512. Point Homebrew, vcpkg,
DEB/RPM inputs and exact-version consumers at that source.

Subsequent recipe/evidence commits must preserve the frozen product and installed
documentation. Any product change requires a new freeze and qualification.
Historical failures, skips and previous releases remain historical records.

## Exact-source qualification

Require full source CI and its Required CI gate, media integrations,
OpenSSL/BCrypt/Mbed TLS profiles, explicit ACP1 partitions, ABI/export checks,
package consumers, Homebrew source installation/audit, six vcpkg profiles and
Linux package qualification. Full CI includes the ecosystem suite; do not launch
release-integrations solely to duplicate that suite on the same source.

Require multicast qualification on all five OS/backend rows without skipped
required cases, including empty-interval key progression, epoll transitions and
plain/CTR/GCM media examples. Preserve child logs on failure. Require real-UDP
ARQ-first tail-loss tests, option-off behavior and missing-feedback limits.
Original Windows failures and their successful later runs remain separately
identified; a rerun does not establish the original failure's cause.

Repeat issue #360's Linux native and full Telemetry/Doctor downstream checks on
the frozen product, retaining exact companion identities, first outcomes and
cleanup evidence. Earlier source or resource-profile exceptions do not qualify
this product. Review the frozen delta against the
[completed 2026-11-04 security scan](security-review-2026-11-04.md); its scope and
revision must not be presented as a scan of later changes.

## Windows upgrade and protected rebuild

Qualify upgrades and backend coexistence from the immutable signed 0.3.1 SDKs.
Ordinary packaging PRs retain variant builds, installer, upgrade and signing-input
regressions. The optional unsigned `qualify_rebuild=true` path is recipe diagnosis
and is not part of the normal release sequence. Do not run pre-tag signing as
well as tag signing.

After accepting frozen-source gates, create the protected annotated `v0.3.2` tag.
Its SDK workflow performs one fresh independent rebuild in the protected
environment. It downloads no ordinary producer artifacts, uses no producer
caches and has no OIDC signing authority. The signer consumes that immutable
artifact ID, verifies provenance, signs and retests the exact bytes. Existing
environment approvals remain required. A failed rebuild must be preserved and
resolved; changed source needs fresh qualification.

## Publication

Keep the release a draft until final-tag CI, packages and protected signing
complete. Verify the live annotated tag object and commit, download and compare
the exact signed assets and checksums, then publish only those qualified bytes.
Record run identities in release evidence and tag provenance without rewriting
installed documentation after freeze. Never move an already published tag.
Preserve 0.3.1 artifacts. Bottle and external package-repository publication have
their own distribution checks.
