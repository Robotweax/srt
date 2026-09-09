# Windows SDK installer (experimental)

This is an SDK for application developers, not a standalone streaming app.
The intended bundle contains static Robotweax SRT and static OpenSSL Crypto for
Debug/Release × Win32/x64/Arm64. Consumers use `/MDd` for Debug and `/MD` for
Release. Debug binaries are development-only; applications must deploy the
appropriate Microsoft runtime for Release. AES-GCM preview is not enabled.

Build each variant using `build-sdk.ps1` in the corresponding Visual Studio
developer environment, supplying matching static OpenSSL headers/library and
its `LICENSE.txt`. Do not mix architectures or CRT profiles. The script refuses
existing build/stage directories. Combine the six SDK variants only after
checking that shared headers and license files are identical.

Compile `sdk.iss` with Inno Setup, supplying `/DSdkRoot=...` and
`/DProductVersion=...`. Do not publish until Windows install/uninstall and all
six consumer link checks pass. This initial packaging work is not a signed or
qualified release installer. No existing release assets are modified.

The `Windows SDK candidate` workflow builds the six variants from a
pinned OpenSSL 3.6.3 revision and uploads short-lived candidate artifacts only.
It links a public C consumer for every target and runs it for Win32/x64; ARM64
execution is not verified by the x64 runner. OpenSSL assembly is enabled using
NASM for Win32/x64 and the `VC-WIN64-CLANGASM-ARM` target (MSVC C compiler,
clang-cl assembler) for ARM64. Missing assemblers or disabled assembly fail the
build; assembler versions and OpenSSL configuration are recorded in CI logs.
This is **not a performance qualification**; production throughput still needs
measurement on the target hardware. This workflow runs manually or for
PRs changing Windows packaging; it does not run on ordinary pushes or
automatically publish release assets.

Interactive installation shows the installer UI. Unattended installation:

```powershell
Start-Process -Wait -FilePath .\robotweax-srt-VERSION-windows-sdk.exe -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'
```

Use `/DIR="C:\custom\Robotweax-SRT"` to select another directory. The installer
sets machine-wide `ROBOTWEAX_SRT`; open a new shell to observe the change.
Import `$(ROBOTWEAX_SRT)\srt.props` after project compiler settings:

```xml
<Import Project="$(ROBOTWEAX_SRT)\srt.props" />
```

Header includes remain `<srt/srt.h>`; the root is private to this SDK and does
not install over a system Haivision installation. OpenSSL Crypto is an explicit
static link dependency. Applications with another OpenSSL dependency must resolve
version/link compatibility; static linkage does not remove symbol conflicts.

The candidate workflow also builds the public C consumer through
`consumer/consumer.vcxproj`, importing the staged `srt.props` for all six
combinations. It executes Win32/x64 consumers and deliberately checks that a
static-CRT profile is rejected. ARM64 remains compile/link-only on this runner.

Upgrades currently require uninstalling the previous SDK first. The installer
rejects an already registered SDK even when a different destination is selected.
CI checks rejection, preservation of the existing props file, uninstall and
reinstallation into another path. This is not an automatic in-place upgrade test.
Do not install
over an older SDK with a different file inventory. Uninstall removes the
environment variable only when it still points to this installation.

References: [Inno Setup command-line options](https://jrsoftware.org/ishelp/topic_setupcmdline.htm),
[OpenSSL Windows build notes](https://github.com/openssl/openssl/blob/openssl-3.6.3/NOTES-WINDOWS.md).

## Preview release preparation (blocked until signing is configured)

`Prepare Windows SDK preview draft` is manual-only and accepts an existing
`windows-sdk-vX.Y.Z-preview.N` tag and a successful manually dispatched **main**
`Windows SDK candidate` run. The tag must resolve to the exact candidate commit.
It does not rebuild the six variants and never publishes a release automatically.
It creates a new draft prerelease containing only the installer and SHA-256 file;
existing releases are not overwritten. No Haivision binaries are included.

Before enabling this process, maintainers must:

1. Select and integrate the Robotweax code-signing service into the candidate
   build, including a trusted timestamp. This integration is **not implemented**
   yet; today's unsigned candidates are deliberately rejected.
2. Configure the `windows-sdk-preview` GitHub environment with required reviewer
   approval and main-only deployment restrictions. Set its variable
   `WINDOWS_SDK_SIGNER_THUMBPRINT` to the approved certificate's 40 hex digits.
   No private signing key belongs in a repository or build artifact.
3. Run the signed candidate workflow on main. Record a clean Windows installation,
   Visual Studio consumer build/run, and uninstall test before public release.
4. Explicitly create the preview tag at that run's commit, then dispatch the draft
   workflow with its run ID and tag before artifacts expire (seven days).
5. Review draft assets, signature, checksums, license bundle, evidence and limitations.
   Publishing the draft requires a separate human decision. Do not attach these
   previews to an existing stable protocol release.

The draft workflow itself verifies the installer signature, timestamp presence and
approved signer. It does not execute the downloaded installer. ARM64 execution,
throughput qualification and signing-service integration remain open work.
