# Package inputs and qualification contract — 0.3.1

This record identifies the immutable product used by the 0.3.1 recipes. It is
separate from the historical [0.3.0 publication record](qualification-0.3.0.md).
Project version **0.3.1**, shared-library ABI **0.3**, compatible SRT API **1.5.7**.
The 85-symbol export inventory preserves all 84 released 0.3.0 C exports and
adds direct group adoption. Source-tree C++ consumers must rebuild.

## Immutable product archive

- Product commit: `ae5750ab75a40d48dff5647231ba20af5cb39c13`.
- Public archive: `https://github.com/Robotweax/srt/archive/ae5750ab75a40d48dff5647231ba20af5cb39c13.tar.gz`.
- Bytes: 2111789.
- SHA-256: `7c6c8130e19b88c72c101ebf8289066774943b2f54971185c368fda5fa63b731`.
- SHA-512: `db9f980e90329c1a3343c97c915c4b5818c36d0f5b52eb0507e1279313edcd1645e232ae313eb33d88ae78f54969530ad473c5eaf67a49d7334dcbdb6ce6429f`.
- All 719 tracked regular file bytes and executable bits matched Git.
- Machine-readable identity and protected product paths: [source-0.3.1.json](source-0.3.1.json).

Homebrew, vcpkg, DEB/RPM and Linux test scripts select this archive, not a moving
branch or uncreated tag. Later recipe/evidence commits preserve every protected
product path, including installed documentation and Windows build/signing recipes.
Verify this before accepting their results:

```sh
git diff --exit-code ae5750ab75a40d48dff5647231ba20af5cb39c13 HEAD -- src include cmake compat CMakeLists.txt docs README.md CHANGELOG.md CONTRIBUTING.md SECURITY.md THIRD_PARTY.md LICENSE examples experimental fuzz benchmarks tools tests interop .github/workflows/windows-sdk.yml packaging/windows
```

## Finished documentation and upgrade inputs

All 90 public Markdown files passed both source and isolated documentation-only
installation checks. Windows variants install that same CMake documentation
component; versioned release notes and the public guides are required in the
installed SDK and covered by its payload hashes. Documentation checks reject a
current release-note/README version mismatch or release-candidate framing while
preserving explicitly historical 0.3.0 evidence. The local native Debug suite
passed 72/72 CTest entries; these macOS observations do not substitute for the
separate exact-source hosted or protected Windows checks.

The previous signed 0.3.0 SDK installers were independently downloaded and hashed
before pinning upgrade tests. Windows validates their Authenticode signatures:

| Backend | SHA-256 |
| --- | --- |
| OpenSSL | `bbdc356da8954cd2d239953f4aef583905714fa651dd4a2b7d8ccb8fcb6138ff` |
| BCrypt | `8ca4319c04373bb20d5b4007df36efede205abbfd15f632563af541c322fa4a9` |

## Release evidence

Use the [release contract](../docs/release-process-0.3.1.md) for full CI,
integrations, provider, package, downstream, resource-scope and security gates.
Hosted workflow completion and final acceptance are established by the exact
source/run identities and published release evidence, not this input record.
A source archive or local test pass is not a release acceptance claim.

The normal release runs the protected independent SDK rebuild once from the
final tag. PRs retain ordinary variant/installer checks. Neither the optional
unsigned recipe diagnostic nor a pre-tag signed run is dispatched for this
release. Signing still requires a fresh protected rebuild, its immutable artifact
ID, source/run/hash checks before OIDC, exact signed-byte retests and the existing
environment approvals. No normal producer artifact or cache is a signing input.

The final draft remains unpublished until tag CI/packages, the protected SDK
workflow, tag provenance and downloaded signed-byte checks pass. Record those
final identities with release assets rather than editing installed documentation
after the expensive rebuild. Preserve all failed/skipped results and historical
resource limits; no new general performance, bottle, PPA or COPR claim is implied.

## Installer preflight correction

The first ordinary SDK installer check at PR head `7ba422fe3` failed for both
backends: its payload guard did not admit the newly installed documentation
subtree. The original [failed run](https://github.com/Robotweax/srt/actions/runs/40029913259)
is retained and is not acceptance evidence. No protected independent rebuild ran.
The corrected product permits only `share\doc\robotweax_srt\` in addition to
the existing payload roots. Traversal, reserved-name, untracked-file and reparse
point rejection remain required. Preflight and small native installer fixtures
now exercise documentation introduction before the ordinary full variant builds.
First-install logs are retained for subsequent diagnostic failures.
