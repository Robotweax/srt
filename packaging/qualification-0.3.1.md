# Package inputs and qualification contract — 0.3.1

This record identifies the immutable product used by the 0.3.1 recipes. It is
separate from the historical [0.3.0 publication record](qualification-0.3.0.md).
Project version **0.3.1**, shared-library ABI **0.3**, compatible SRT API **1.5.7**.
The 85-symbol export inventory preserves all 84 released 0.3.0 C exports and
adds direct group adoption. Source-tree C++ consumers must rebuild.

## Immutable product archive

- Product commit: `adbf8a7a161f59aa56a2f15482173dd635cc3a09`.
- Public archive: `https://github.com/Robotweax/srt/archive/adbf8a7a161f59aa56a2f15482173dd635cc3a09.tar.gz`.
- Bytes: 2114163.
- SHA-256: `5a3095e4093982f3a890590c2a8ef9d2b073fb4c8c846181102e8e7ba087faa9`.
- SHA-512: `d98fd980ff183986df0bd7ad1e7347dae8b59c02956f31de4be10932861ee3ba33d3cb3c6feb46ce61d621ec606c298e2c5d28cc8fae489c3683acaffcb19da8`.
- All 721 tracked regular file bytes and executable bits matched Git.
- Machine-readable identity and protected product paths: [source-0.3.1.json](source-0.3.1.json).

Homebrew, vcpkg, DEB/RPM and Linux test scripts select this archive, not a moving
branch or uncreated tag. Later recipe/evidence commits preserve every protected
product path, including installed documentation and Windows build/signing recipes.
Verify this before accepting their results:

```sh
git diff --exit-code adbf8a7a161f59aa56a2f15482173dd635cc3a09 HEAD -- src include cmake compat CMakeLists.txt docs README.md CHANGELOG.md CONTRIBUTING.md SECURITY.md THIRD_PARTY.md LICENSE examples experimental fuzz benchmarks tools tests interop .github/workflows/windows-sdk.yml packaging/windows
```

## Finished documentation and upgrade inputs

All 90 public Markdown files passed both source and isolated documentation-only
installation checks. Windows variants install that same CMake documentation
component; versioned release notes and the public guides are required in the
installed SDK and covered by its payload hashes. Documentation checks reject a
current release-note/README version mismatch or release-candidate framing while
preserving explicitly historical 0.3.0 evidence. The local native Debug suite
passed 72/72 CTest entries; these macOS observations do not substitute for the
separate exact-source hosted or protected Windows checks.

The previous signed 0.3.0 SDK installers were independently downloaded and hashed
before pinning upgrade tests. Windows validates their Authenticode signatures:

| Backend | SHA-256 |
| --- | --- |
| OpenSSL | `bbdc356da8954cd2d239953f4aef583905714fa651dd4a2b7d8ccb8fcb6138ff` |
| BCrypt | `8ca4319c04373bb20d5b4007df36efede205abbfd15f632563af541c322fa4a9` |

## Release evidence

Use the [release contract](../docs/release-process-0.3.1.md) for full CI,
integrations, provider, package, downstream, resource-scope and security gates.
Hosted workflow completion and final acceptance are established by the exact
source/run identities and published release evidence, not this input record.
A source archive or local test pass is not a release acceptance claim.

The normal release runs the protected independent SDK rebuild once from the
final tag. PRs retain ordinary variant/installer checks. Neither the optional
unsigned recipe diagnostic nor a pre-tag signed run is dispatched for this
release. Signing still requires a fresh protected rebuild, its immutable artifact
ID, source/run/hash checks before OIDC, exact signed-byte retests and the existing
environment approvals. No normal producer artifact or cache is a signing input.

The final draft remains unpublished until tag CI/packages, the protected SDK
workflow, tag provenance and downloaded signed-byte checks pass. Record those
final identities with release assets rather than editing installed documentation
after the expensive rebuild. Preserve all failed/skipped results and historical
resource limits; no new general performance, bottle, PPA or COPR claim is implied.

## Installer preflight correction

The first ordinary SDK installer check at PR head `7ba422fe3` failed for both
backends: its payload guard did not admit the newly installed documentation
subtree. The original [failed run](https://github.com/Robotweax/srt/actions/runs/40029913259)
is retained and is not acceptance evidence. No protected independent rebuild ran.
The corrected product permits only `share\doc\robotweax_srt\` in addition to
the existing payload roots. Traversal, reserved-name, untracked-file and reparse
point rejection remain required. Preflight and small native installer fixtures
now exercise documentation introduction before the ordinary full variant builds.
First-install logs are retained for subsequent diagnostic failures.

## Preserved full-source qualification outcomes

At integrated main `1637651b4757d81e32627f7fe32f51159e992cc2`, the first
[full CI attempt](https://github.com/Robotweax/srt/actions/runs/40034733105/attempts/1)
failed during macOS OBS desktop automatic encrypted reconnect: an endpoint exited
before decoded media. The saved logs did not identify which endpoint or its exit
code. The first [ACP1 attempt](https://github.com/Robotweax/srt/actions/runs/40034682845/attempts/1)
failed in Linux Backup Group Rendezvous during second-member replacement with
CTR/192. Its first 256 bidirectional messages were uncorrupted; replacement did
not connect within the original deadline. Both original failures remain failed.
Subsequent attempts initiated outside this release agent passed on unchanged
source; they do not establish the causes of the first failures.

A [focused OBS diagnostic run](https://github.com/Robotweax/srt/actions/runs/40069921017)
then failed the unchanged idle allocation bound (2 versus 1) before streaming.
That failure remains retained. A [second diagnostic cohort](https://github.com/Robotweax/srt/actions/runs/40071109903)
passed three strict fresh-profile OBS desktop runs, including encrypted A/V,
automatic reconnect, source forwarding and normal shutdown. Its isolated ACP1
job passed 25 repetitions of the preserved CTR/192 Backup Rendezvous replacement
case, recording four distinct UDP endpoints. Production library sources were
unchanged in these diagnostic branches. Diagnostic-only job selection is not
full-CI acceptance and is excluded from the frozen product.

The final harness correction reserves all four group Rendezvous endpoints during
selection. Previously the first pair was released before selecting replacements,
so a legal ephemeral-port allocator could reuse an endpoint. A deterministic
adversarial-allocator regression rejects that old code. Historical ports were
not recorded, so this is a proven harness flaw, not a proven explanation of the
first ACP1 failure. Failure logs now retain caller/listener exits and arguments.
macOS OBS failures also retain endpoint PIDs/exits and bounded PID-matched text
crash reports. No transfer, media, reconnect, shutdown, allocation or deadline
assertion was relaxed. All original CI gates remain present.

These corrections change qualification tooling only. Library, installed public
documentation, ABI/version headers and Windows SDK build/signing recipes are
unchanged from the prior integrated product. Affected checks and final-source
acceptance remain required. No new production OBS application or supported ACP1
security claim is introduced.
