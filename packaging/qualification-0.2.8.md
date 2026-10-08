# Package qualification 0.2.8

Status: **candidate recipes qualified; final publication gates pending**.

Product source: `8126e166ecd3d33987b3748e799ef40e5192d068`.
Archive SHA-256: `f3ad362970d8b83337bd8344f796337a56ff52347c2a9e2ff18c41fd2b71422d`.
Archive SHA-512: `cc5d8d25f124254d6a249c515296d9b22491a29e4fc18c2db5c64379f86278c15f3ada31caec659ffaa38b3434fa59d3f280a4c57c1ce5aa3bdf0636a187f2e1`.

All 587 regular files and executable bits in the downloaded GitHub
archive were compared with the immutable Git source. These hashes describe the
downloaded bytes, not a local git archive. The product source is separate from
the recipe/evidence commit. Product changes require a new source pin and repeat
qualification. Previous release results do not qualify this candidate.

## Accepted candidate qualification

Recipe head `240713f759d20e717ef2f855e12815d12f964b29`, reviewed in
[PR #287](https://github.com/Robotweax/srt/pull/287), preserves the frozen
product tree. The following runs completed successfully at that exact head:

- [Source CI](https://github.com/Robotweax/srt/actions/runs/37798431450): configured
  source, C exports/installed consumers, sanitizer, fuzz and reference gates.
- [Complete ecosystem suite](https://github.com/Robotweax/srt/actions/runs/37798459745):
  configured FFmpeg, GStreamer, VLC and OBS headless/desktop integrations.
- [BCrypt qualification](https://github.com/Robotweax/srt/actions/runs/37798464386):
  configured Windows profiles; optional native ARM64/performance diagnostics
  were skipped and are not qualification evidence.
- [Package managers](https://github.com/Robotweax/srt/actions/runs/37798431594):
  Homebrew source install/test/audit, standalone consumers and coexistence/removal;
  six vcpkg static/dynamic profiles on Linux x64, Windows x64 and macOS arm64,
  including Debug/Release consumers, relocation and coexistence/removal.
- [Linux packages](https://github.com/Robotweax/srt/actions/runs/37798431434):
  Ubuntu 24.04 and Fedora 44 binary/source builds, source/license checks and
  installed-consumer/coexistence/removal qualification.
- [Unsigned Windows SDK](https://github.com/Robotweax/srt/actions/runs/37798468880):
  twelve variants and installation/coexistence checks. This run selected
  `sign=false`; its protected rebuild and signing jobs were skipped. It does
  not qualify signed installers or the independent rebuild recipe.

## Final publication gates

Final main/tag CI, independently rebuilt and signed Windows SDK assets, exact
signed-byte tests and downloaded draft-byte verification remain required.
The separate tap/native bottle requires its own qualification before publication.
Record exact tag/source identities, successful final run links and artifact
hashes as those gates complete. Preserve original failures; exclude failed,
cancelled and superseded attempts from accepted results.

Recipe workflows build the pinned archive, not the moving protocol checkout.
Clean coexistence/removal checks do not establish upgrade/rollback, PPA/COPR,
registry availability or all-platform bottles. Historical
[0.2.7 qualification](qualification-0.2.7.md) remains unchanged.
See [candidate release notes](../docs/release-notes-0.2.8.md) and the
[release process](../docs/release-process-0.2.8.md).
