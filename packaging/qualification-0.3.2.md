# Package inputs and qualification contract — 0.3.2

The 0.3.2 recipes select the immutable product below. Project version is
**0.3.2**, shared-library ABI **0.3**, compatible SRT API **1.5.7**. All 85 C
exports from 0.3.1 are preserved. The historical
[0.3.1 record](qualification-0.3.1.md) remains unchanged.

## Immutable product archive

- Product commit: `838a022b75cfa95137bb7e40f575d0a65f6bd551`.
- [Public archive](https://github.com/Robotweax/srt/archive/838a022b75cfa95137bb7e40f575d0a65f6bd551.tar.gz).
- Bytes: 2239489.
- SHA-256: `b7f0cb906abe8d064e7d7c18287596ff0f594ac6cb07aaa10f5c791971683a9b`.
- SHA-512: `0c8fe50aed576628c84a677a51009438365ed1d6bec63b84ac0c632903e97a8ba2bb6f8bfd3ced1f6109f2522e31db9d74d859b6f99d50b006d93e0a5a1090b3`.
- All 752 tracked regular files matched Git byte-for-byte, including executable bits.
- Machine-readable identity and frozen paths: [source-0.3.2.json](source-0.3.2.json).

Homebrew, vcpkg, DEB/RPM and the Linux qualification workflow use this exact
archive. Recipe and evidence commits must preserve all frozen product paths:

```sh
git diff --exit-code 838a022b75cfa95137bb7e40f575d0a65f6bd551 HEAD -- src include cmake compat CMakeLists.txt docs README.md CHANGELOG.md CONTRIBUTING.md SECURITY.md THIRD_PARTY.md LICENSE examples experimental fuzz benchmarks tools tests interop .github/workflows/windows-sdk.yml packaging/windows
```

## Local verification and upgrade inputs

The integrated product includes the optional srt-live-transmit recipe from
PR #377. All 96 public Markdown files passed source and isolated installed
documentation validation. Library sources, public headers, build configuration,
compatibility contracts and native examples are unchanged from the preceding
`b19c6a88b6257c749e16dc1e53d803cc3c792e32` freeze. That earlier archive is
superseded; its qualification remains historical evidence for those unchanged
paths, not exact-source qualification of this integrated product.

The complete
native macOS Release/OpenSSL/GCM build passed 140/140 CTest entries after the
protocol, test and documentation corrections and before the version-only update.
That result is local correction evidence, not final-source release acceptance.
The subsequent header, ABI and installed-consumer checks passed 3/3; the AEAD,
release-documentation, Windows-SDK and release-tag harness checks passed 28/28.
A final documentation consistency pass replaced stale current-version and
source-review status text. The initial archive at `4b464a789521b1ce5b6b045f934eca7364660135`
is superseded by the product above; it is not final release evidence.

The published signed 0.3.1 SDK assets are the previous-release inputs. Their
SHA-256 values were checked against GitHub release asset digests; Windows
qualification downloads and hashes the files and verifies Authenticode:

| Backend | SHA-256 |
| --- | --- |
| OpenSSL | `644574ee4f76058cd9125f7ea8617a425f12b3f48ac94d56344a28557de259fa` |
| BCrypt | `5c06888bf60c5ffeaa84ae084a8393020292ba587b46e0d2ca300218a179b26c` |

## Required release evidence

The [release contract](../docs/release-process-0.3.2.md) requires full frozen-source
CI, all multicast OS/backend rows, ARQ transport cases, media integrations,
provider/ACP1 matrices, installed-package checks and exact-source Linux downstream
qualification. Hosted completion is established by exact run/source identities,
not by the existence of this input record. Preserve initial failures and skips.

The [completed security source review](../docs/security-review-2026-11-04.md)
covers the earlier recorded revision; the subsequent key-lifecycle corrections
have separate regression evidence. ARQ-first is regular opt-in. Multicast's
trusted-group, revocation and physical-network qualification limits remain.

Windows upgrade and coexistence checks must pass against the pinned 0.3.1 assets.
The protected independent rebuild runs once from the accepted final annotated
tag. Do not duplicate it with pre-tag signing or the optional unsigned rebuild
diagnostic. Existing environment approvals and signed-byte retests still apply.
Keep the release draft until final-tag CI/packages, signing, provenance and
download verification pass. This record does not claim publication, a qualified
new Homebrew bottle, PPA/COPR availability or general performance guarantees.
