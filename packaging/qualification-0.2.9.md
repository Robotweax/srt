# Package qualification 0.2.9

Status: **qualified and published on 2026-10-13**.
Release: [v0.2.9](https://github.com/Robotweax/srt/releases/tag/v0.2.9), published
at 09:46:16 UTC and marked latest. Separate bottle publication remains pending.
Tag commit `a87779d9c3d831583d32b2f61cae5b81edb05537`, protected annotated
object `2e1055a4733c62adfe4de7369d2fb0fd17c8f450`.

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
the frozen product and executable recipe inputs. Final tag CI and SDK below
select the exact tag commit, while package workflows consume the frozen archive.

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
| Twelve SDK variants, both installers and actual 0.2.8 upgrade | Passed, including separate candidate rebuild | `00f8e1b`: [SDK candidate](https://github.com/Robotweax/srt/actions/runs/38352569751); twelve builds, two installer tests and coexistence pass |
| Protected independent rebuild, signing and exact signed-byte tests | Passed | `a87779d`: [final SDK](https://github.com/Robotweax/srt/actions/runs/38365080105), independent rebuild, signing, exact signed installation tests and draft verification all pass |
| Final tag source/package/SDK CI and downloaded draft comparison | Passed and published | `a87779d`: [source/full media](https://github.com/Robotweax/srt/actions/runs/38365080090), [managers](https://github.com/Robotweax/srt/actions/runs/38365080086), [Linux](https://github.com/Robotweax/srt/actions/runs/38365080088), [SDK](https://github.com/Robotweax/srt/actions/runs/38365080105); exact draft and public bytes verified |
| Separate native Homebrew bottle qualification/publication | Qualified; maintainer review/publication pending | [Tap #8](https://github.com/Robotweax/homebrew-tap/pull/8), exact head `33147063`; [native CI](https://github.com/Robotweax/homebrew-tap/actions/runs/38355338385) and downloaded bottle verified; no 0.2.9 bottle published |

Only actual successful exact-SHA runs are accepted. Skipped, cancelled,
superseded or failed attempts remain excluded. Final tag provenance and the
post-signing digests below were verified against downloaded draft bytes before
publication, then rechecked against the actual public downloads.

## Final tag and signed download acceptance

All final source/SDK jobs select `a87779d9c3d831583d32b2f61cae5b81edb05537`.
Source/full media CI passed 52 jobs and Required CI, with one optional
Live-timing diagnostic skipped. The manager suite passed all seven jobs;
Linux packages passed both Ubuntu 24.04 and Fedora 44 jobs. The SDK passed
19 jobs: validation, twelve builds, both installers, coexistence, independent
protected rebuild, signing and draft verification. Its unprotected candidate
rebuild job is intentionally skipped for tag events; it does not substitute
for the passed protected rebuild.

The independently rebuilt unsigned installers were passed by exact artifact
identity, source/run/version and pre-signing hashes to the protected signer.
The exact signed executables passed installation, signed 0.2.8 upgrade,
repair/rejection, coexistence and independent removal. Native verification
confirms Robotweax GmbH as publisher, trusted timestamps and unchanged
post-signing bytes. Draft verification independently repeats signature and
manifest checks before release upload.

Signed artifact `11801367380` contains exactly both executables and `SHA256SUMS`.
Downloaded ZIP: 64,259,333 bytes, SHA-256
`f4b0b54bda04ac1d71a29df93bf9c5b9ed966b540874d7bdc0f7b172ec6be05a`.
The independently downloaded draft and public files match that artifact byte
for byte; GitHub asset sizes/digests and the exact two-entry manifest agree.

| Delivered asset | Bytes | SHA-256 |
| --- | ---: | --- |
| `robotweax-srt-0.2.9-windows-sdk-openssl.exe` | 51,002,976 | `9cc066dfee3278319d552bbfe47d48b39c285b7512f7fd836fad00241216cdd1` |
| `robotweax-srt-0.2.9-windows-sdk-bcrypt.exe` | 14,356,176 | `d063d68f4ae4d977b31ef129947c78bc3abc2947ae3e94a5ec28abfce0117c04` |
| `SHA256SUMS` | 219 | `2a2d8a3dd36114fcda2d961965242f7900dacc4584a22eb252d7ad109e2f0ab7` |
| `tag-provenance.json` | 127 | `4fecbfae7974081cc8edf3f6c147fd1b2b582d1504914997671c9b72dbdaa6d6` |

Downloaded provenance equals the independently read protected annotated tag
object and commit above. The release has exactly these four assets. Annotation
is not a GPG signature; the accepted Windows signatures are Authenticode.
The source-tag documentation retains its pre-publication status; this main
record and the release page document actual publication without changing the
published tag.

The first unpublished tag/source attempt `38357547646` failed installed
Markdown validation because #314 introduced a relative link to noninstalled
packaging documentation. One-line #315 corrected the link and passed both
static/shared installed checks without changing product/ABI/recipes. The old
SDK attempt `38357547699` was cancelled before protected rebuild/signing/draft.
With explicit maintainer approval, only unpublished `refs/tags/v0.2.9` was
corrected under an exact old-object lease. Original active update/delete rules
for `v*` were restored immediately, with no exclusions or bypass actors.
Old tag object `b021060de696ad92b6eeb30b41e74098feb91db2` and failed/cancelled
attempts remain in retained audit evidence, excluded from acceptance.
No test guard was weakened. The published tag must never be moved or recreated.

## Separate bottle qualification

Tap #8 remains open at `33147063d1a17ea300f22de63fc167be0a3e4134`.
The downloaded ARM64 bottle from artifact `11792618027` is 686,411 bytes,
SHA-256 `eb36adf2e6440c3113385ef04e259840c4cc0ed0bba4695d338576d2245ed47c`.
Its embedded formula/header match the canonical frozen package recipe and
0.2.9 version; the library is ARM64 Mach-O with the exact 84 C exports.
Native qualification uses macOS 15.7, Xcode 16.4 and OpenSSL 3.6.5.
This qualifies the particular downloaded bottle, not other platforms.
Maintainer review and publication through the tap's guarded process remain
separate; no published 0.2.9 bottle is claimed.

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

See [release notes](../docs/release-notes-0.2.9.md),
[release process](../docs/release-process-0.2.9.md), and the unchanged historical
[0.2.8 record](qualification-0.2.8.md).
