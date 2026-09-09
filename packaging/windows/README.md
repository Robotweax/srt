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
execution is not verified by the x64 runner. The dependency baseline uses
`no-asm` for initial reproducibility across the targets and is **not a performance
qualification**. Optimized dependency builds must be addressed before recommending
these candidates for production throughput. This workflow runs manually or for
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
