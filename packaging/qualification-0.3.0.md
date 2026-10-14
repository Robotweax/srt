# Package qualification 0.3.0

Status: **candidate prepared; qualification pending**. Nothing is published.
Project version **0.3.0**, shared-library ABI line **0.3**, compatible SRT API
**1.5.7**. Existing applications and dependencies must rebuild for ABI line 0.3.
Previous [0.2.9 acceptance](qualification-0.2.9.md) remains unchanged.

## Frozen product and archive verification

- Product commit: `1e7d7824a0420dac8e8ccaa950668ee301f99b9f`.
- Downloaded public archive: `https://github.com/Robotweax/srt/archive/1e7d7824a0420dac8e8ccaa950668ee301f99b9f.tar.gz`.
- Bytes: 1931583.
- SHA-256: `046813c64242afec5fc1393790f04094cb4c8eecb3a769b49eed77632046901b`.
- SHA-512: `d8de39577bb61ce5ec6a934e6596c4e0735dd6e38870b40903ad95378a1f3bb8990d97dcd9305681c7faa9ed7878f03ed8d74fc32c8be171d90b496088e5d088`.
- All 639 tracked regular files and executable bits match Git.
- The frozen 0.3.0 export inventory contains 84 symbols and preserves
  every prior release's declared C exports.

The archive is downloaded GitHub content, not a locally generated substitute.
Later recipe/evidence commits must preserve `src`, `include`, `cmake`, `compat`
and `CMakeLists.txt` relative to the frozen product. Product changes require a
new pin and repeated package qualification. Recipe inputs point to this exact
archive rather than a moving branch or an uncreated release tag.

## Candidate acceptance

| Gate | State | Evidence required |
| --- | --- | --- |
| Local static Debug and shared Release | Passed | Frozen product `1e7d7824a`: 69/69 static and 20/20 shared checks; includes exact 84 exports, installed C/C++ consumers, ABI 0.3 and installed documentation |
| Full source CI | Pending | Exact-head Required CI and all selected full platform, sanitizer, fuzz and interoperability jobs |
| Complete ecosystem integrations | Pending | Explicit release-integrations run including every OBS platform/desktop job |
| Homebrew and six vcpkg profiles | Pending | Actual source install/test/audit and standalone/coexistence/relocation/removal checks |
| Ubuntu 24.04 / Fedora 44 | Pending | Pinned-archive DEB/RPM source and installed-consumer qualification |
| Windows SDK | Pending | Twelve variants, both installers, actual 0.2.9 upgrade, repair/rejection/coexistence and independent reconstruction |
| Protected signing | Not started | Main-only approved independent rebuild, trusted publisher/timestamp and exact signed-byte tests |
| Native Homebrew bottle | Not started | Separate tap review, native qualification and downloaded digest verification |
| Resource and final security review | Pending | Same-product workload evidence and review of available scan findings and unresolved limits |

Local acceptance also passed 806 interoperability Python tests (through CTest),
17 tooling tests, six API inventory checks, AEAD contract checks, all 82 installed
Markdown documents, pinned clang-format 22.1.8, action pin policy, shell/Ruby
syntax and whitespace checks. These local macOS results do not replace the
pending hosted platform/package/signing gates.

## Upgrade fixture provenance

The downloaded published signed 0.2.9 installer bytes were independently hashed
before updating the 0.3.0 upgrade fixtures:

| Installer | SHA-256 |
| --- | --- |
| OpenSSL | `9cc066dfee3278319d552bbfe47d48b39c285b7512f7fd836fad00241216cdd1` |
| BCrypt | `d063d68f4ae4d977b31ef129947c78bc3abc2947ae3e94a5ec28abfce0117c04` |

Windows CI must additionally verify their Authenticode signatures and execute
the real upgrade. Download/hash verification on macOS is not upgrade acceptance.

OpenSSL remains the package-manager default. Mbed TLS is a separately selected
provider; ACP1 remains experimental and disabled in normal packages. No new
PPA, COPR, registry, bottle or application distribution is implied. The
[candidate notes](../docs/release-notes-0.3.0.md) and
[release process](../docs/release-process-0.3.0.md) preserve the exact limits.

## Superseded preliminary freeze

Preliminary source `526ad4f32c80dbe999ec758350118d1f33082266` and its archive are excluded from acceptance.
Its first local runs passed 67/69 static and 18/20 shared checks; installed
consumer examples and a release-contract test still requested version 0.2.
The corrected source pin above includes those fixture updates. Original logs
and downloaded bytes are retained; the initial results are not passing evidence.
