# Package qualification 0.2.8

Status: **qualification pending; not published**.

Product source: `8126e166ecd3d33987b3748e799ef40e5192d068`.
Archive SHA-256: `f3ad362970d8b83337bd8344f796337a56ff52347c2a9e2ff18c41fd2b71422d`.
Archive SHA-512: `cc5d8d25f124254d6a249c515296d9b22491a29e4fc18c2db5c64379f86278c15f3ada31caec659ffaa38b3434fa59d3f280a4c57c1ce5aa3bdf0636a187f2e1`.

All 587 regular files and executable bits in the downloaded GitHub
archive were compared with the immutable Git source. These hashes describe the
downloaded bytes, not a local git archive. The product source is separate from
the recipe/evidence commit. Product changes require a new source pin and repeat
qualification. Previous release results do not qualify this candidate.

## Pending acceptance

- Exact-head source CI, C exports/installed consumers, sanitizers and reference
  interoperability; full release integrations and experimental BCrypt profiles.
- Homebrew source install, test/audit, standalone consumers, coexistence/removal.
- Six vcpkg static/dynamic profiles on Linux x64, Windows x64 and macOS arm64,
  with Debug/Release consumers, relocation, coexistence/removal.
- Ubuntu 24.04 and Fedora 44 binary/source builds, source/license checks and
  installed-consumer/coexistence/removal qualification.
- Final main/tag CI and independently rebuilt, signed and retested Windows SDK
  assets, including comparison of downloaded draft bytes before publication.
- Separate Homebrew tap/native bottle qualification before bottle advertisement.

No run is recorded as accepted yet. Record exact source/recipe/tag commits,
successful run links, downloaded artifact hashes and remaining limitations here
as qualification completes. Preserve original failures and exclude failed,
cancelled or superseded attempts from acceptance.

Recipe workflows build the pinned archive, not the moving protocol checkout.
Clean coexistence/removal checks do not establish upgrade/rollback, PPA/COPR,
registry availability or all-platform bottles. Historical
[0.2.7 qualification](qualification-0.2.7.md) remains unchanged.
See [candidate release notes](../docs/release-notes-0.2.8.md) and the
[release process](../docs/release-process-0.2.8.md).
