# Package qualification 0.2.9

Status: **candidate recipes prepared; qualification and publication pending**.
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

## Acceptance inventory

| Gate | Status | Accepted source/recipe SHA and run |
| --- | --- | --- |
| Exact-head source CI and Required CI | Pending | Not recorded |
| Explicit complete ecosystem integrations | Pending | Not recorded |
| Homebrew source installation/test/audit and consumers | Pending | Not recorded |
| Six vcpkg static/shared platform profiles | Pending | Not recorded |
| Ubuntu 24.04 source/binary packages and coexistence/removal | Pending | Not recorded |
| Fedora 44 SRPM/binary packages and coexistence/removal | Pending | Not recorded |
| Experimental Windows BCrypt source/crypto matrix | Pending | Not recorded |
| Twelve SDK variants, both installers and actual 0.2.8 upgrade | Pending | Not recorded |
| Protected independent rebuild, signing and exact signed-byte tests | Pending | Not recorded |
| Final tag source/package/SDK CI and downloaded draft comparison | Pending | No tag/draft asset accepted |
| Separate native Homebrew bottle qualification/publication | Pending | No 0.2.9 bottle accepted |

Update only with actual successful exact-SHA run URLs and retained evidence.
Skipped, cancelled, superseded or failed attempts are not accepted results.
Final tag provenance and post-signing SHA-256 values must be recorded before
publication. Keep the release draft until final acceptance.

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
