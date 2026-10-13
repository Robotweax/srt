# Package qualification 0.2.9

Status: **preparation merged; source/media/package/resource acceptance passed;
final SDK rebuild, tag, signing and bottle publication pending**.
Latest published release remains v0.2.8. No v0.2.9 tag has been accepted.

## Frozen product source

- Product commit: `80873657469344becb9c35f5306d0050ec5a566d`.
- Downloaded archive: `https://github.com/Robotweax/srt/archive/80873657469344becb9c35f5306d0050ec5a566d.tar.gz`.
- Bytes: 1809831.
- SHA-256: `16753663e52f77e9795b462927147b1d47ac48719d7a4d7fb10cd1db28f80c1b`.
- SHA-512: `284bcac0ccc2c910af25d781dd10a4e725283100a7e33fab3cfa8cea8883c97c2c67c5006ffddeae5fcba3c48d0ceff1c62fa29b5ac4e4702b5a6285a547d056`.
- All 608 regular files and executable bits verified against Git.

These digests identify the downloaded public GitHub bytes, not a local Git
archive. Product source is separate from recipe/evidence commits. Product
changes require a new pin and repeated package qualification. Preserve
`src`, `include`, `cmake`, `compat` and `CMakeLists.txt` relative to this source.
Previous-release results do not qualify this candidate.

Recipe commit: `f7b33bc2a9deaf69ecf4a0711608c7235ed781ae`. Later acceptance/link records preserve
the frozen product and executable recipe inputs; exact final CI heads must
still be recorded independently.

## Local candidate verification

The frozen product passed macOS arm64 static Debug CTest **73/73** and shared
Release CTest **20/20**, including installed C/C++ consumers, public exports,
ABI and shared lifecycle checks. The public documentation checker verifies all
77 installed Markdown files. The four additive observation getters retain
all prior exports; the new snapshot has exactly 84 symbols. Historical ABI
and 0.2.8 release records are byte-identical to the development baseline.

The API/version/ABI, CI-selection and tag guards pass 102 targeted controls.
An initial direct module invocation could not import the schedule test's sibling
helper; that log is retained. The same 18 schedule controls pass using their
normal unittest discovery invocation. No test rule or deadline was changed.
Ruby formula syntax, Linux shell syntax and cross-recipe source/version/digest
checks pass. The recipe/evidence changes preserve the frozen product tree.
These local checks do not qualify Windows, Linux package managers, full media
integrations, performance, signing or final-tag publication.

## Candidate and main acceptance

Preparation [#312](https://github.com/Robotweax/srt/pull/312) merged as
`ce168060c241097b8af5b0cd9e8898027fb0ff09` after the required gate passed on
`00f8e1b9b36862776d522f07fc5f5239c56ccfc1`. The complete merge tree equals
that candidate, and the product paths equal the frozen source above.
The up-to-date branch includes only the reviewed Windows artifact-action pin
from #309. The first update lacked its DCO trailer; that failed attempt is
retained and the tree-identical signed update passes. No test gate was weakened.

## Acceptance inventory

| Gate | Status | Accepted source/recipe SHA and run |
| --- | --- | --- |
| Exact-head source CI and Required CI | Passed on candidate and main | `00f8e1b`: [candidate CI](https://github.com/Robotweax/srt/actions/runs/38352569707); `ce168060`: [main CI](https://github.com/Robotweax/srt/actions/runs/38353900709), 44 success / 9 conditional skips |
| Explicit complete ecosystem integrations | Passed | `00f8e1b`, tree-identical to merge: [complete suite](https://github.com/Robotweax/srt/actions/runs/38352598006), including both OBS desktop profiles |
| Homebrew source installation/test/audit and consumers | Passed | Frozen product and recipe above; [candidate managers](https://github.com/Robotweax/srt/actions/runs/38352569737), [main managers](https://github.com/Robotweax/srt/actions/runs/38353900605), all seven jobs pass |
| Six vcpkg static/shared platform profiles | Passed | Same exact source/recipe and manager runs; all six profiles pass |
| Ubuntu 24.04 source/binary packages and coexistence/removal | Passed | [candidate Linux packages](https://github.com/Robotweax/srt/actions/runs/38352569818), [main Linux packages](https://github.com/Robotweax/srt/actions/runs/38353900613), both jobs pass |
| Fedora 44 SRPM/binary packages and coexistence/removal | Passed | Same immutable source/recipe and Linux package runs |
| Experimental Windows BCrypt source/crypto matrix | Passed for configured profiles | `00f8e1b`: [BCrypt](https://github.com/Robotweax/srt/actions/runs/38352569731); optional ARM64 runtime and serial-performance diagnostics remain skipped |
| Twelve SDK variants, both installers and actual 0.2.8 upgrade | Passed; separate rebuild still pending | `00f8e1b`: [SDK candidate](https://github.com/Robotweax/srt/actions/runs/38352569751); twelve builds, two installer tests and coexistence pass |
| Protected independent rebuild, signing and exact signed-byte tests | Pending | Not recorded |
| Final tag source/package/SDK CI and downloaded draft comparison | Pending | No tag/draft asset accepted |
| Separate native Homebrew bottle qualification/publication | Running separately | [Tap #8](https://github.com/Robotweax/homebrew-tap/pull/8), exact canonical formula; no 0.2.9 bottle accepted or published |

Update only with actual successful exact-SHA run URLs and retained evidence.
Skipped, cancelled, superseded or failed attempts are not accepted results.
Final tag provenance and post-signing SHA-256 values must be recorded before
publication. Keep the release draft until final acceptance.

## Selected resource and longer-running acceptance

The frozen 0.2.9 product passed 40 prospectively fixed resource comparisons
against published 0.2.8 and two 300-second own-provider loopback runs. All
42 original outcomes and 588 manifest entries were independently verified.
Each soak transfers exactly 6,000,033,536 bytes, with both peer exits zero.
Paired median CPU/Gbit increase is at most 3.19%, summed process-peak RSS
increase at most 0.55%, and throughput loss at most 0.002%; all satisfy the
unchanged 15% CPU/RSS and 10% throughput limits.

The [compact resource record](performance-0.2.9.json) pins the source,
baseline, compiler, harness, declaration and private archive digest. It
records the selected four resource profiles and CTR/GCM-FEC-SESSIONAUTH
soaks with key rotation. These results do not qualify WAN/NIC/24-hour or
all-platform behavior, general Group selection performance or Windows
BCrypt performance. No raw archive was publicly uploaded.

## Contracts and limits

Project version 0.2.9, C ABI 0.2 and compatible SRT API 1.5.7 remain separate.
The four observation getters are additive, with a new exact export snapshot;
historical snapshots remain frozen. OpenSSL/AES-CTR defaults and explicit
feature opt-ins are unchanged; BCrypt stays experimental.

Package-manager recipes use the exact archive and namespaced layout. They do
not alter an installed third-party application or promise in-process provider
coexistence. Standalone, filesystem coexistence, relocation where applicable,
and independent removal require actual manager tests on supported platforms.

SDK upgrade inputs are the published, signed 0.2.8 installers with fixed
digests. Recipe regression tests do not substitute for actual installed
0.2.8-to-0.2.9 consumer and signed-0.2.9 checks. No new PPA/COPR, official
distribution admission, third-party application package or unqualified bottle
is advertised.

See [candidate release notes](../docs/release-notes-0.2.9.md),
[release process](../docs/release-process-0.2.9.md), and the unchanged historical
[0.2.8 record](qualification-0.2.8.md).
