# Release process for Robotweax SRT 0.3.5

This is the pre-freeze runbook for 0.3.5. The comparison baseline is the
published `v0.3.4` tag at `87742777290e6a77e38672ca35701f7184b75526`.
The scope below was reviewed through upstream `main` at
`31475429875f0604c83fe60676f7653876742579` on 23 November 2026. The product
commit, release date, qualification results and release decisions are pending.
The expected version is 0.3.5, with ABI 0.3 and compatible SRT API 1.5.7.
See the [0.3.4 release contract](release-process-0.3.4.md) and the
[changes since the previous tag](https://github.com/Robotweax/srt/compare/v0.3.4...31475429875f0604c83fe60676f7653876742579).

## 1. Confirm scope and freeze the product

- [ ] Review the final delta from `v0.3.4` and settle release scope. The current
  delta includes default timer pacing on Android and iOS, opt-in pacing
  coalescing, fractional pacer and File rate-control intervals, earlier recovery
  from late timer wakes, and same-pass rechecking of isolated one-packet gaps
  before a NAK. It also covers option resets, group option validation and
  Windows DLL process-exit handling. Include the associated test and harness
  changes in qualification.
- [ ] Update CMake and the project version header, changelog, release notes,
  current-version documentation and exact-version consumers. Document
  `ROBOTWEAX_SRT_MOBILE_PACING=legacy` and the deprecated settings in the mobile
  upgrade guidance. Keep the documented limits for experimental profiles.
  Verify the 0.3.5 ABI snapshot against the 86 exports in 0.3.4, including a
  C/C++ header and layout check for changed public headers. Check the installed
  documentation and consumers in an empty prefix.
- [ ] Review the security-sensitive source delta and existing findings. Record
  disposition for peer-authentication admission (issue 406), macOS identity
  Rendezvous timeouts and OBS desktop reconnect. Record any accepted residual
  risk with an owner decision scoped to this release and commit. Review the
  first-failure evidence in issue 407 and the reference group caller race in
  issue 417 alongside the post-0.3.4 test and harness changes.
  [Peer-authentication admission](https://github.com/Robotweax/srt/issues/406),
  [hosted test failures](https://github.com/Robotweax/srt/issues/407),
  [reference group caller race](https://github.com/Robotweax/srt/issues/417).
- [ ] Before freeze, update the Windows upgrade test to use the published
  0.3.4 SDK for each backend. Check its Authenticode status and SHA-256 against
  the published asset and the previous qualification record. Update the
  synthetic upgrade checks for the new installed documentation version.
  [Published 0.3.4 release](https://github.com/Robotweax/srt/releases/tag/v0.3.4).
- [ ] Freeze one product commit after scope and review are complete. Download
  the commit-addressed GitHub archive and verify its tracked file bytes and
  executable bits against Git. Record its commit, URL, byte count, SHA-256,
  SHA-512 and protected paths in `packaging/source-0.3.5.json`. Point
  Homebrew, vcpkg, DEB and RPM at that exact archive. Confirm every subsequent
  recipe or evidence commit preserves the frozen paths. Any change to those
  paths restarts the freeze and qualification.

## 2. Qualify the frozen source

- [ ] Dispatch full `ci.yml` with default inputs on the release branch after
  each push and verify its head SHA. Require the **Required CI gate** and
  execution of the complete ecosystem suite (srt-live-transmit, FFmpeg,
  GStreamer, VLC and OBS, including desktop jobs). Skip the separate
  release-integrations dispatch when full CI already covers the same SHA.
  A path-selected PR run or multicast-only dispatch has partial coverage.
  Record the actual Linux runner and container identities
  for the Ubuntu 26.04 jobs.
- [ ] Require the OpenSSL, BCrypt and Mbed TLS rows, ACP1, hybrid Release and
  peer-authentication profiles, GCC/Clang sanitizer rows, and ABI and installed
  package consumers. Verify the required multicast OS/backend jobs and native
  hybrid, ARQ-first, null packet deletion and group interoperability cases.
  Run a commit-bound hybrid soak with recorded bounds and retain separate
  native macOS results for tests excluded on hosted macOS. Repeat downstream
  Telemetry and Doctor validation against the same product.
- [ ] Verify mobile timer default and legacy builds with the pinned Android
  and iOS toolchains, including the iOS simulator build. On physical Android
  and iOS, compare default timer and legacy on the same profile and device.
  Record toolchain, device and OS, offered and delivered rates, sender CPU time,
  and pacing behavior. Run the opt-in coalescing cases separately with the
  quantum recorded. Keep CPU, delivery and energy claims scoped to what was
  measured. Follow the [mobile build and measurement guide](mobile-building.md).
- [ ] Exercise the new timing and reliability paths on the frozen source.
  Cover fractional pacing and File loss decrease, coarse-timer recovery,
  same-pass NAK deferral and recovery, and the direct and experimental batch
  receive paths. Verify group admission and option resets, Rendezvous deadlines,
  and Windows DLL shutdown alongside the ecosystem tests.
- [ ] Run Linux packages (`linux-packages.yml`) and package managers
  (`package-managers.yml`) on the release ref that selects the frozen
  archive. Require DEB/RPM build and install, Homebrew source installation and
  audit, and all six vcpkg profiles. Verify exact-version package consumption.
- [ ] Dispatch `windows-sdk.yml` on the release ref with both inputs at their
  defaults (`sign=false`, `qualify_rebuild=false`). Require validation, all
  twelve build variants, both installers, installed consumers, 0.3.4 upgrades,
  repair, coexistence and independent uninstall. The rebuild, signing and
  release draft jobs are reserved for the final tag.
- [ ] Record runs in `packaging/qualification-0.3.5.md`, including workflow,
  ref and SHA, run and attempt IDs, required job results, skips and their
  reason, runner and toolchain identity, and artifacts. Preserve first-failure
  logs before selective retries, including hangs and cancelled siblings. Keep
  the first failure and its disposition with the eventual result. A passing
  retry establishes that the retry passed. A root-cause claim needs evidence.

## 3. Merge, tag and run the protected Windows rebuild

- [ ] Confirm the frozen-source evidence and any explicit risk decisions before
  merging the release change. Verify the merged tree preserves the qualified
  frozen paths and selects the qualified package inputs. A merge that changes
  those inputs returns to qualification before tagging.
- [ ] After merge and owner approval, create the protected annotated `v0.3.5`
  tag on the verified merge commit. Record the product SHA, release SHA and
  annotated tag object ID. The tag run performs the single planned Windows
  rebuild from source on a fresh protected runner with read-only permissions.
  The signer verifies the rebuild artifact's identity and provenance before
  signing and retesting that pair. Complete the existing
  `windows-release-signing` environment approvals before the protected jobs
  run. Leave manual signing and manual rebuild qualification disabled
  throughout the pre-tag phase.
- [ ] If signing or draft creation fails after the rebuild succeeds, rerun only
  failed jobs while the immutable rebuild artifact remains available (seven
  days). If the rebuild itself fails or its artifact expires, record the reason
  and retain the original run evidence before repeating the rebuild. Keep the
  release tag fixed.

## 4. Verify and publish

- [ ] Require final-tag full CI, package workflows and the protected Windows
  SDK run. Verify that the draft contains the OpenSSL and BCrypt installers,
  `SHA256SUMS` and `tag-provenance.json`. Download the draft assets and match
  their bytes and digests to the signed CI artifact. Verify both Authenticode
  signatures and timestamps. Check the live annotated tag object and peeled
  release SHA with `tools/verify_release_tag.py --repo Robotweax/srt --tag
  v0.3.5 --commit <release-sha> --object <annotated-tag-object>`.
- [ ] Attach the reviewed notes to the draft and publish after the release
  owner accepts the final evidence. Re-download the public assets and verify
  digests and provenance again. Record the final tag, runs and artifact IDs in
  the qualification record while preserving the frozen documentation.
- [ ] Qualify the Homebrew tap and bottle separately, including upgrade from
  the published 0.3.4 bottle and a digest check on the public bottle. Apply the
  distribution-specific qualification before publishing to other channels.
