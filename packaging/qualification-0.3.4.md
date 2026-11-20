# Package inputs and qualification — 0.3.4

The owner directed the product freeze on 20 November 2026. This record names the
frozen product and what has and has not been run on it. It does not authorize
publication. Project version is 0.3.4, ABI is 0.3, compatible API is 1.5.7; all
85 C exports from 0.3.3 are preserved and `robotweax_srt_epoll_wake` is added.
Historical qualification records remain unchanged.

## Frozen product

- Product commit: `6009c6430b46e8691181bbd8690bf36c3ef0d661`.
- [Public archive](https://github.com/Robotweax/srt/archive/6009c6430b46e8691181bbd8690bf36c3ef0d661.tar.gz); 3343283 bytes.
- SHA-256: `66ab41f65523fce7cea66c5ac54b44f3f8d7b5a87586755b00ee91a70d870116`.
- SHA-512: `2b9b6e7c80fad9247f9baffc6628df3fda81d742fd1991ec01ddfc13784d97027188bb06643670b0ad81f9cc9fd1c85cd85853fa8d0502d0f067523b00e06824`.
- All 993 regular files and executable bits verified against Git; the archive
  holds no other file; two downloads were byte-identical.
- [Machine-readable identity and frozen paths](source-0.3.4.json);
  [accepted risk](acceptance-0.3.4.json).
- `dev/release-0.3.4-product` advertises this commit.

The recipe commit that adds this record selects the archive in the Homebrew,
vcpkg, DEB and RPM recipes and leaves every frozen path unchanged.

## Evidence before the freeze

These runs are on the predecessor `a3a926ae2aa208aa56b85f020dece6490aa15b77`,
not on the product. The product differs from it in one frozen file,
`interop/tests/test_ci_changes.py`, which now expects the 0.3.4-1 Debian
changelog entry, and in `packaging/debian/changelog`.

- [Full CI, 44628848242](https://github.com/Robotweax/srt/actions/runs/44628848242): 64 jobs passed, 1 skipped, on the
  first attempt.
- OBS Windows under MSVC 19.52.37029 (image 20261111.635): the pinned OBS
  source fails there on its own C4244 warning. First failure:
  [run 44622775025](https://github.com/Robotweax/srt/actions/runs/44622775025/job/135908940906) on `3701ea3`. `a3a926a` builds
  the OBS source without warnings as errors. Attempts 1 and 2 of the full CI
  run above were assigned MSVC 19.51.36260, which never failed, and establish
  nothing about the change. In attempt 3 "OBS Windows integration" ran on
  19.52.37029 and passed with the warning reported as a warning. "OBS Windows
  desktop" has not been observed on 19.52; it uses the same build script.

## Results on the frozen product

Recipe commit `fe91e0a` selects the archive and leaves every frozen path
unchanged. All runs on it succeeded on the first attempt:

- [Full CI, 44692277103](https://github.com/Robotweax/srt/actions/runs/44692277103) with the Required CI gate and the
  complete ecosystem suite: 64 jobs passed, 1 skipped ("Live timing
  interoperability"). "OBS Windows desktop" ran on MSVC 19.52.37029 and
  "OBS Windows integration" on 19.51.36260; with attempt 3 of the pre-freeze
  run, both OBS Windows jobs have now passed once on 19.52.
- [Linux packages, 44692283146](https://github.com/Robotweax/srt/actions/runs/44692283146) and
  [Package managers, 44692283008](https://github.com/Robotweax/srt/actions/runs/44692283008) from the exact archive.
- [Windows SDK, 44692283043](https://github.com/Robotweax/srt/actions/runs/44692283043): validation, build matrix,
  installers and coexistence, 16 jobs. `rebuild`, `rebuild-qualification`,
  `sign` and `release-draft` were skipped as intended; the from-source rebuild
  is reserved for the final tag and runs once, see the
  [release contract](../docs/release-process-0.3.4.md).
- Pull-request CI 44692283106, hybrid control 44692283265 (includes the Linux
  strict soak), ACP1 44692283019, BCrypt 44692283017 and peer authentication
  44692283086.

[Local macOS hybrid soak](soak-0.3.4-macos.json): passed on a clean checkout of
the product commit, Release/OpenSSL, CTR and GCM at 30 seconds each.

Local macOS arm64 Debug `ctest` on the frozen tree: 97 of 97 passed.

## Not yet run on the frozen product

- Downstream Lab smoke, interoperability and telemetry requests.
- The protected rebuild, signing and release draft, which belong to the tag.

## Open on this product

- Peer-authentication listener admission is serialised; accepted as a
  documented limit on 19 November 2026 and tracked in
  [issue 406](https://github.com/Robotweax/srt/issues/406).
- The residual risks accepted for 0.3.3 (macOS identity-rendezvous timeout,
  intermittent OBS desktop reconnect) carry over unchanged by the owner's
  decision of 20 November 2026. No correction for either is part of this
  product; neither is marked fixed.
- Three corrections from the [0.3.4 delta disposition](../docs/security-delta-0.3.4.md)
  have no test that observes them: expected-tag erasure, batch receive
  hardening and the group enrolment rollback.
- UDP batching build, Linux container, before the freeze: `remote_harness_tests`
  failed with its output not captured, and was not compared against the
  unchanged tree. Cause not established.
