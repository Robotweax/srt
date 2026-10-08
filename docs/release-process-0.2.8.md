# Release process for Robotweax SRT 0.2.8

Status: **preparation and qualification in progress; not published**.
The development baseline is `ffa61fb912c59e0bf49c8709c4bb00fd396ea128`.
Historical [0.2.7 acceptance](release-process-0.2.7.md) remains unchanged.

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
