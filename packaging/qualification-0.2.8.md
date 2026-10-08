# Package qualification 0.2.8

Status: **qualified and published on 2026-10-08**.
Release: [v0.2.8](https://github.com/Robotweax/srt/releases/tag/v0.2.8).
Tag commit `09ceab33e1fc190472558fdb4d873f38b76fa320`, annotated object `890715013facb86d6425c052ca22acf73a3436af`.

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

## Final acceptance and publication

- [Final main CI](https://github.com/Robotweax/srt/actions/runs/37808164740) and [full tag CI](https://github.com/Robotweax/srt/actions/runs/37809908884) passed at `09ceab33e1fc190472558fdb4d873f38b76fa320`.
- [Tag package managers](https://github.com/Robotweax/srt/actions/runs/37809908815) passed Homebrew and all six vcpkg profiles; [Linux packages](https://github.com/Robotweax/srt/actions/runs/37809908927) passed Ubuntu 24.04 and Fedora 44 builds, consumers and coexistence/removal.
- [Final SDK workflow](https://github.com/Robotweax/srt/actions/runs/37809908833) built twelve variants, tested both installers and coexistence, independently rebuilt both installers on the approved protected runner, then signed and retested the exact pair. Downloaded executables and their manifest match the qualified signed artifact byte for byte.
- [Native bottle qualification](https://github.com/Robotweax/homebrew-tap/actions/runs/37803883653) and [tap publication](https://github.com/Robotweax/homebrew-tap/actions/runs/37822519622) passed. Tap main `60d220b9737e65ab73674427d2b2af6fa79284f8` selects the canonical formula plus bottle metadata; the downloaded arm64_sequoia bottle matches the qualified artifact and formula hash.

Post-signing installer SHA-256:

- `robotweax-srt-0.2.8-windows-sdk-openssl.exe`: `dda7da8e9e212161fe87b92e7ec791aa5a08da3fae5935ad6d54b70c5c1834f4`
- `robotweax-srt-0.2.8-windows-sdk-bcrypt.exe`: `99eb8ec845164daa5e3310fc7262aa0179682f9860969a52dfc02691de4be533`

The [published arm64_sequoia bottle](https://github.com/Robotweax/homebrew-tap/releases/tag/robotweax-srt-0.2.8) has SHA-256 `1058b2cf39d26f19afff857dc5b1886f29ad7909459231c27af568274a875396`. Other bottle platforms remain unqualified.

Original candidate failures remain excluded from acceptance: a relative link to noninstalled packaging documentation was corrected; one macOS OBS desktop reconnect attempt exited before decoded media. Its unchanged candidate retry and the first final-tag attempt passed. The original reconnect cause remains undetermined. Full logs/artifacts are retained; no transport or test guard was weakened. Optional live-timing/native ARM64/performance CI jobs are separate diagnostics and are not inferred from the accepted platform builds.

Recipe workflows build the pinned archive, not the moving protocol checkout.
Clean coexistence/removal checks do not establish upgrade/rollback, PPA/COPR,
registry availability or all-platform bottles. Historical
[0.2.7 qualification](qualification-0.2.7.md) remains unchanged.
See [release notes](../docs/release-notes-0.2.8.md) and the
[release process](../docs/release-process-0.2.8.md).
