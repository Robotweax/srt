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

Downstream Lab requests on the product commit all returned PASS with archive
validation and cleanup verified, using Haivision
`899348d8318eb9a3c5a5b6ec43c4a1114288773a`, Telemetry
`d03ca4527b005be0cfbe542ddaac6c0ede5e7955`, Doctor
`88f8e33851f4fa975ab1b708e9f707d27505a399` and the telemetry endpoint profile:
[smoke](https://github.com/Robotweax/srt_network_lab/actions/runs/44698180743), [interop](https://github.com/Robotweax/srt_network_lab/actions/runs/44698184590), [telemetry](https://github.com/Robotweax/srt_network_lab/actions/runs/44698188691).

Record commit `cb39772` changes only files under `packaging`.
[Full CI, 44697891134](https://github.com/Robotweax/srt/actions/runs/44697891134) on it passed on the first attempt:
64 jobs passed, 1 skipped.

## First failure on the frozen product

[Pull-request CI 44697896434](https://github.com/Robotweax/srt/actions/runs/44697896434/job/136159029880) on `cb39772`
failed in "AES-GCM platform/ABI (macos-latest)":
`robotweax_srt_application_recovery-arqfirst_ctr` delivered 159 of 160
messages. The receiver reported one belated packet, one receive drop and a
largest delivery gap of 288.6 ms against the sample's 120 ms latency. The
remaining 49 example tests of the job passed, including the GCM variant of the
same scenario. The Windows leg of the matrix was cancelled by the failure and
the Required CI gate of that run failed with it. The run was not re-run.

- The same job passed on the same commit in full CI 44697891134, and in both
  CI runs on `fe91e0a`, whose frozen paths are identical.
- Local macOS arm64, Release build of the product commit: 80 consecutive
  repetitions of the test passed.
- No other isolated failure of this test is among the last 100 CI runs.
- [Full CI, 44704113041](https://github.com/Robotweax/srt/actions/runs/44704113041) on the following record commit
  `be05e63` passed on the first attempt, this job included: 64 jobs passed,
  1 skipped.

Hosted comparison,
[run 44704672021](https://github.com/Robotweax/srt/actions/runs/44704672021): the test alone, repeated on
`macos-latest` with the configuration of the failing job, 6 jobs of 200
iterations for each source. The workflow is the only change of commit
`fa561d673e8f68a2cdec104f3e556db314b46452` on the temporary branch
`diag/arqfirst-macos-0.3.4`; the logs of the failing iterations are in the
run's artifacts, which expire after 14 days.

| Source | Iterations | Failures | Largest delivery gap of each failure |
|---|---|---|---|
| Product commit `6009c64` | 1,200 | 1 | 133.9 ms |
| `v0.3.3` (`69bd532`) | 1,200 | 3 | 196.6, 202.9 and 220.8 ms |

- All four failures have the signature of the first one: 159 of 160 messages,
  one receive drop, a delivery gap above the 120 ms latency.
- The failure therefore exists on `v0.3.3` under the same conditions and is
  not introduced by 0.3.4. The counts are too small to compare the two rates;
  they show no increase on 0.3.4 and do not prove the rates equal.
- Still not established: whether the late delivery originates in the hosted
  runner or in the library. Nothing measured here separates the two, and the
  failure has not been reproduced locally. The test leaves no margin for a
  delay of this size on hosted macOS and can fail again, in this job or on
  the tag.

## Second first failure: Windows helper-test timeout

Record commit `c408520` changes only this file.
[Full CI, 44715753633](https://github.com/Robotweax/srt/actions/runs/44715753633) on it passed on the first attempt:
64 jobs passed, 1 skipped, with the Linux, macOS and Windows Release jobs.

[Pull-request CI 44715761217](https://github.com/Robotweax/srt/actions/runs/44715761217/job/136218713106) on the same
commit failed in "windows-latest Release" on image 20261103.629:
`robotweax_srt_application_process_support` reached its 20-second timeout.
The other 110 tests of the job passed.

- The test is Python only. It exercises the example process helper (log
  capture and port planning) and does not load the library. Neither the test
  nor the helper differs from `v0.3.3`.
- Attempt 2 re-ran the job and it passed. That does not establish the cause.
- "macos-latest Release" was cancelled by the Windows failure before it was
  assigned a runner and did not execute in any of the three attempts, so the
  Required CI gate of that run failed in each. The job passed on the same
  commit in the full CI run above.
- Cause not established. The timeout left no test output. Earlier runs were
  not searched for the same timeout.

## Merge, tag and publication

This section was added after publication. It records what followed record
commit `ff3c7fc` and changes no earlier statement.

On `ff3c7fc`, [full CI 44726538408](https://github.com/Robotweax/srt/actions/runs/44726538408) passed on the first attempt.
[Pull-request CI 44726544653](https://github.com/Robotweax/srt/actions/runs/44726544653) failed in its first attempt in
"windows-latest Release": `robotweax_srt_application_process_support` timed out
again, after 21.62 seconds. As before, the failure cancelled
"macos-latest Release", which did not execute in the re-run either, so that
run's gate stayed failed.

The owner directed the merge with that check red. Pull request 405 was merged
on 20 November 2026 as `87742777290e6a77e38672ca35701f7184b75526`; its tree is
identical to `ff3c7fc`.

On the merge commit:

- [CI 44735673903](https://github.com/Robotweax/srt/actions/runs/44735673903) passed on the second attempt. In the first,
  "macos-latest Release" failed in
  `robotweax_srt_application_recovery-arqfirst_ctr`: 159 of 160 messages, a
  largest delivery gap of 256.1 ms.
- Package managers 44735673734, Linux packages 44735673798, peer
  authentication 44735673806, hybrid control 44735674389 and ACP1 44735673790
  passed on the first attempt.

Annotated tag `v0.3.4`, object `559c0c4a0ba0ddbb0d65a62eebbe127f5187f2a5`, points
at the merge commit. On the tag:

- [CI 44743035936](https://github.com/Robotweax/srt/actions/runs/44743035936) passed on the second attempt. In the first,
  "srt-live-transmit / windows-latest" failed on image 20261111.635: the built
  application started with `-version` did not return within 60 seconds. Only
  that job and the gate were re-run. The same job had passed on the same
  commit and image in the merge-commit run.
- Linux packages 44743035891 and Package managers 44743035923 passed on the
  first attempt.
- [Windows SDK installers 44743035885](https://github.com/Robotweax/srt/actions/runs/44743035885) passed in one attempt:
  validation, the twelve-variant matrix, installers, coexistence, `rebuild`,
  `sign` and `release-draft`. `rebuild-qualification` was skipped. This is the
  only from-source rebuild run for 0.3.4; it took 81 minutes.

Published assets, 20 November 2026:

- `robotweax-srt-0.3.4-windows-sdk-openssl.exe`, SHA-256
  `73bf1fe6510d96ddf01e048fa7d1833fbb8b5ffc277c02ceec7d218c0f8805dc`.
- `robotweax-srt-0.3.4-windows-sdk-bcrypt.exe`, SHA-256
  `ac638a864f1891a09ad3c01eb1872224a36f616c8cd9f8a89c38c7be71023df6`.
- Both downloaded installers are byte-identical to signed CI artifact
  `14640243579` and match `SHA256SUMS`. `tag-provenance.json` names the tag
  object and commit above. The Authenticode and timestamp validation was done
  by the `release-draft` job; it was not repeated outside CI.

Homebrew: the tap formula was updated in
[homebrew-tap pull request 14](https://github.com/Robotweax/homebrew-tap/pull/14). Native qualification
[44768944062](https://github.com/Robotweax/homebrew-tap/actions/runs/44768944062) passed on the first attempt, with the
upgrade from the published 0.3.3 bottle, 86 exports and 106 installed
documents. [Publication 44772229425](https://github.com/Robotweax/homebrew-tap/actions/runs/44772229425) published the
`arm64_sequoia` bottle, SHA-256
`585b1aaa343416eaff3aec2bb7d1aac06d0280e811eaa6be00936ce76b743ce4`. The public
bottle is byte-identical to the qualified artifact, and its provenance names
that publication run. No vcpkg registry, PPA or COPR is published.

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
- Three intermittent hosted test failures have no established cause and are
  tracked in [issue 407](https://github.com/Robotweax/srt/issues/407): the macOS ARQ-first recovery sample, the
  Windows process-helper timeout and the Windows `srt-live-transmit -version`
  hang.
- UDP batching build, Linux container, before the freeze: `remote_harness_tests`
  failed with its output not captured, and was not compared against the
  unchanged tree. Cause not established.
