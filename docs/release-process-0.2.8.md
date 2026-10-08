# Release process for Robotweax SRT 0.2.8

Status: **completed and published on 2026-10-08**.
Release: [v0.2.8](https://github.com/Robotweax/srt/releases/tag/v0.2.8).
Protected annotated tag object `890715013facb86d6425c052ca22acf73a3436af` selects `09ceab33e1fc190472558fdb4d873f38b76fa320`. This published tag must not be moved or recreated.

## Completed acceptance record

Preparation [PR #287](https://github.com/Robotweax/srt/pull/287) was normally merged after exact-head candidate qualification. The final product tree equals the independently verified frozen package source; version/API/default profiles remain unchanged.

- [Final main CI](https://github.com/Robotweax/srt/actions/runs/37808164740) and [full tag CI](https://github.com/Robotweax/srt/actions/runs/37809908884) passed at `09ceab33e1fc190472558fdb4d873f38b76fa320`.
- [Tag package managers](https://github.com/Robotweax/srt/actions/runs/37809908815) passed Homebrew and all six vcpkg profiles; [Linux packages](https://github.com/Robotweax/srt/actions/runs/37809908927) passed Ubuntu 24.04 and Fedora 44 builds, consumers and coexistence/removal.
- [Final SDK workflow](https://github.com/Robotweax/srt/actions/runs/37809908833) built twelve variants, tested both installers and coexistence, independently rebuilt both installers on the approved protected runner, then signed and retested the exact pair. Downloaded executables and their manifest match the qualified signed artifact byte for byte.
- [Native bottle qualification](https://github.com/Robotweax/homebrew-tap/actions/runs/37803883653) and [tap publication](https://github.com/Robotweax/homebrew-tap/actions/runs/37822519622) passed. Tap main `60d220b9737e65ab73674427d2b2af6fa79284f8` selects the canonical formula plus bottle metadata; the downloaded arm64_sequoia bottle matches the qualified artifact and formula hash.

Original candidate failures remain excluded from acceptance: a relative link to noninstalled packaging documentation was corrected; one macOS OBS desktop reconnect attempt exited before decoded media. Its unchanged candidate retry and the first final-tag attempt passed. The original reconnect cause remains undetermined. Full logs/artifacts are retained; no transport or test guard was weakened. Optional live-timing/native ARM64/performance CI jobs are separate diagnostics and are not inferred from the accepted platform builds.

The following procedure records the completed release. Commands creating refs or publishing artifacts are historical instructions for this version.

The development baseline is `ffa61fb912c59e0bf49c8709c4bb00fd396ea128`.
Historical [0.2.7 acceptance](release-process-0.2.7.md) remains unchanged.

## Frozen source and recipe qualification

Product source: `8126e166ecd3d33987b3748e799ef40e5192d068`. Its downloaded GitHub archive
was verified against all 587 regular files and executable bits.
SHA-256: `f3ad362970d8b83337bd8344f796337a56ff52347c2a9e2ff18c41fd2b71422d`.
SHA-512: `cc5d8d25f124254d6a249c515296d9b22491a29e4fc18c2db5c64379f86278c15f3ada31caec659ffaa38b3434fa59d3f280a4c57c1ce5aa3bdf0636a187f2e1`.
The [package record](https://github.com/Robotweax/srt/blob/main/packaging/qualification-0.2.8.md)
records accepted candidate/final recipe, platform and publication evidence. Archive verification alone does not establish final acceptance.

## Candidate CI evidence

At recipe head `240713f759d20e717ef2f855e12815d12f964b29`,
[source CI](https://github.com/Robotweax/srt/actions/runs/37798431450) and the
[explicit full ecosystem suite](https://github.com/Robotweax/srt/actions/runs/37798459745)
passed both Required CI gates. The package record identifies successful
Homebrew, vcpkg, Linux and BCrypt runs at that head. Later evidence-only commits
preserve the product tree; affected checks still apply to their exact heads.

## Candidate contract

Project version 0.2.8, C ABI line 0.2 and compatible SRT API 1.5.7 remain
separate. Retain OpenSSL/AES-CTR defaults, explicit GCM/MAXREXMITBW opt-ins
and default-off session authentication. Review [candidate release notes](release-notes-0.2.8.md)
and the known security/profile limitations before acceptance.

Freeze the product/version commit before updating recipes. Record its full SHA,
download its GitHub source archive and compare every regular file and executable
bit with Git. Record SHA-256 and SHA-512 of those downloaded bytes. Later
recipe/evidence commits must preserve `src`, `include`, `cmake`, `compat` and
`CMakeLists.txt` relative to that product commit. A product change invalidates
the source pin and requires new archive/package qualification.

## Required qualification

1. Exact-head source CI and Required CI gate: configured platform Debug/Release,
   static/shared installed consumers, C exports, sanitizers, fuzz smoke and
   pinned reference interoperability. Preserve original failures and investigate
   timing-dependent File/Rendezvous and aggregate-deadline failures.
2. Explicit `release-integrations.yml` on the final candidate branch:
   complete configured FFmpeg, GStreamer, VLC and OBS headless/desktop profiles.
   Routine PR/main checks can skip these integrations and do not substitute.
3. Explicit `windows-bcrypt.yml` where not triggered automatically. Native ARM64
   execution and serial performance are separately selected diagnostics;
   cross-compilation is not native execution evidence.
4. Repeated Live/File recovery with loss/delay/reordering, final File packet,
   payload integrity, reconnect, key rotation and supported group profiles.
   Retain strict traffic, payload, exit, NAK/RTO and benchmark checks.
5. Final-candidate CPU/RSS/throughput and longer-running tests for the selected
   workloads. Record host, source/build/peer pins, offered load and limits;
   do not infer NIC/WAN or all-platform qualification from a short loopback lab.
6. Review the user-started Security Cloud run on the baseline and any new
   findings against the frozen product. Keep scan status and finding state
   separate from implemented mitigations; do not describe a running scan as
   passed. No automatic closure of #112 or #190.

Associate all runs with immutable head SHAs. Skipped, cancelled, superseded or
failed attempts are not accepted results. Retain their evidence.

## Package source and recipes

In a separate recipe commit, update Homebrew version/URL/SHA-256, vcpkg
version/URL/filename/SHA-512, Debian changelog, Fedora version/source/extraction,
both Linux qualification scripts, Linux workflow pin/hash/label and the exact
package-consumer version. Update recipe documentation and a new 0.2.8 package
qualification record; preserve previous records.

Require Homebrew source install/test/audit, all six configured vcpkg profiles
and Ubuntu 24.04/Fedora 44 build, installed-consumer, source/license,
coexistence/removal checks against the pinned source archive. Recipe CI builds
that archive, not the moving protocol checkout. Record source/recipe SHAs and
successful run links before acceptance.

## Merge, tag, SDK and publication

After review and candidate gates, merge preparation normally with the
expected-head guard and existing branch protection. Reconfirm final main CI,
version axes, product-tree equality and package pins. Record the final commit.
Create a new protected annotated `v0.2.8` only after candidate acceptance;
never move or recreate a published tag.

Require final tag CI/packages and the Windows SDK workflow. Its existing
`windows-release-signing` approval gate remains in force. Independently rebuilt
and signed OpenSSL/BCrypt installers must pass exact signed-byte installer,
coexistence/removal, trusted publisher/timestamp and post-signing checksum checks.
Compare downloaded draft assets with the qualified artifacts before publication.
Keep the release a draft until final acceptance.

The separate Homebrew tap/bottle update requires its own native qualification
and downloaded-byte/hash verification. Do not advertise an unqualified bottle.
No PPA/COPR, registry, upgrade/rollback or third-party application distribution
is added by this process. Finish records with actual commits, run URLs, hashes,
date and remaining limitations, replacing pending claims only with evidence.
