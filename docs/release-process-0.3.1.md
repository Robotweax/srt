# Release contract for Robotweax SRT 0.3.1

Version 0.3.1 is the integrated main line, including connection-publication and
metadata-retirement fixes and receive/close/file/capture hardening. The
[release notes](release-notes-0.3.1.md) describe its user-facing scope and limits.
The public ABI remains 0.3 and compatible SRT API remains 1.5.7.

## Freeze documentation with the product

Finish the version headers, 85-symbol export snapshot, current API/option guides,
changelog, release notes and Windows SDK guide before freezing product source.
Validate relative links and install the documentation component into an empty
prefix. Windows SDK builds install and validate that same component; installer
tests require its version-matched release notes and public guides.
Historical 0.3.0 failures and dated preparation records remain explicitly
historical; they must not be rewritten as passing checks.

Freeze an immutable public source commit, compare the downloaded archive's tracked
file bytes and executable bits with Git, and record SHA-256/SHA-512. Update every
package recipe, Linux qualification input and exact-version consumer to that
archive. Later recipe/evidence commits must preserve the frozen product and
installed documentation. A change to either invalidates earlier qualification.

## Exact-source gates

Require full source CI and its Required CI gate, all media integrations,
OpenSSL/BCrypt/Mbed TLS profiles, explicit experimental ACP1 partitions, package
consumers and exports, Homebrew source install/audit, six vcpkg profiles, Linux
packages and signed-0.3.0 Windows upgrade/coexistence checks. Full CI already
includes the complete ecosystem suite; do not dispatch release-integrations
merely to duplicate it on the same source.

Repeat issue #360's Linux native and full Telemetry/Doctor downstream checks on
the final integrated product. Retain first outcomes, original failures and skips,
raw private evidence, exact companion identities and cleanup. Previous passing
results qualify only their original source. Available security findings and the
frozen-source delta require review. Resource and extended-profile results need
explicit scope acceptance; the 0.3.0 exception is not automatically transferred.

## One protected independent Windows rebuild

Packaging PRs retain ordinary variant builds, installer, upgrade and signing-input
regressions. They do not automatically run the expensive independent rebuild.
The optional manual `qualify_rebuild=true` path is unsigned recipe diagnosis and
is not part of the normal release sequence; do not dispatch it for this release.
Do not perform a pre-tag `sign=true` run as well as a tag signing run.

After accepting the frozen source and all pre-tag gates, create the new protected
annotated `v0.3.1` tag. Its SDK workflow performs the one fresh, protected
independent rebuild, tests the unsigned pair and records exact source/run/hashes.
The rebuild has read-only repository access and no OIDC authority, downloads no
ordinary producer artifacts and uses no producer caches. The signer consumes
only its immutable artifact ID, verifies provenance before OIDC, signs and retests
the exact bytes. Existing environment approvals remain required.

A failed rebuild is not a pass. Preserve it, fix the cause and reassess the source;
a changed product needs new qualification even when that costs another run.
The once-only sequence removes avoidable duplicates, not failure handling or
signing provenance. Do not move a published tag to repair a failure.

## Publication evidence

Keep the release a draft until final tag CI, packages and protected SDK signing
complete. Verify the live annotated tag object and commit, download and compare
the exact signed assets and checksums, and publish only the qualified bytes.
Record final run identities in the release's evidence and tag-provenance asset;
qualification does not require editing installed user documentation after the
build. The release notes describe the finished product without claiming future
workflow results as passed.

Preserve published 0.3.0 tags, assets and evidence. Separate Homebrew bottle or
package-repository publication requires its own native distribution checks.
