# Windows SDK installer (experimental)

This is an SDK for application developers, not a standalone streaming app.
It implements the packaging work requested in
[issue #12](https://github.com/Robotweax/srt/issues/12).
Two separate installers offer OpenSSL and the
[experimental BCrypt backend](../../docs/windows-bcrypt.md).
Manual target-machine acceptance and
release signing remain pending; this is not a qualified release installer.
The intended bundle contains static Robotweax SRT for
Debug/Release × Win32/x64/Arm64. Consumers use `/MDd` for Debug and `/MD` for
Release. Debug binaries are development-only; applications must deploy the
appropriate Microsoft runtime for Release. AES-GCM preview is not enabled.

Build each variant using `build-sdk.ps1` in the corresponding Visual Studio
developer environment. The default `-CryptoBackend openssl` requires
`-OpenSSLRoot` with matching static OpenSSL headers/library and its `LICENSE.txt`.
Use `-CryptoBackend bcrypt` without `-OpenSSLRoot` for a native Windows package;
this disables OpenSSL discovery and ships no OpenSSL library or license file.
For example, from a matching x64 developer shell:

```powershell
./packaging/windows/build-sdk.ps1 -Platform x64 -Configuration Release -CryptoBackend bcrypt -OutputRoot C:/srt-sdk-bcrypt
```

Do not mix architectures or CRT profiles. The script refuses
existing build/stage directories. Combine the six SDK variants only after
checking that shared headers and license files are identical. Each variant's
`build.json` records its backend; `package.ps1` rejects mixed backends and OpenSSL
files in BCrypt inventories. Distribute `srt-backend.props` alongside `srt.props`;
it supplies the package's backend-specific link dependencies.

Compile `sdk.iss` with Inno Setup, supplying `/DSdkRoot=...`,
`/DProductVersion=...` and `/DCryptoBackend=openssl` or `bcrypt`.
The supplied SDK must match this backend. Do not publish until Windows install/uninstall and all
six consumer link checks pass. This initial packaging work is not a signed or
qualified release installer. No existing release assets are modified.

The `Windows SDK installers` workflow builds six variants per backend and uploads
short-lived candidate artifacts. OpenSSL uses a pinned 3.6.3 dependency; BCrypt
skips that dependency entirely. Package regression tests reject mixed or
incorrectly labelled backend inventories.
It links a public C consumer for every target and runs it for Win32/x64; ARM64
execution is not verified by the x64 runner. OpenSSL assembly is enabled using
NASM for Win32/x64 and the `VC-WIN64-CLANGASM-ARM` target (MSVC C compiler,
clang-cl assembler) for ARM64. Missing assemblers or disabled assembly fail the
build; assembler versions and OpenSSL configuration are recorded in CI logs.
This is **not a performance qualification**; production throughput still needs
measurement on the target hardware. This workflow runs manually or for
PRs changing Windows packaging, and on `v*` tags, but not ordinary branch pushes.
Tags must exactly match `v` plus the CMake project version. Only after both
installers and their side-by-side tests pass are both executables and
`SHA256SUMS` attached to a draft release. Publishing remains a manual review step;
an existing published release is never modified. Re-running an upload with
existing asset names fails rather than replacing them. Manual runs only produce
CI artifacts and are the qualification path before tagging. No tag is needed
to test packaging. Installers are currently unsigned.

Interactive installation shows the installer UI. Unattended installation:

```powershell
Start-Process -Wait -FilePath .\robotweax-srt-VERSION-windows-sdk-openssl.exe -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'
```

Use `/DIR="C:\custom\Robotweax-SRT"` to select another directory. The installer
sets machine-wide `ROBOTWEAX_SRT_OPENSSL` or `ROBOTWEAX_SRT_BCRYPT`; open a new
shell to observe the change. Default folders are `Robotweax-SRT-openssl` and
`Robotweax-SRT-bcrypt` under Program Files. Select exactly one backend by importing
its props after project compiler settings (replace OPENSSL with BCRYPT as needed):

```xml
<Import Project="$(ROBOTWEAX_SRT_OPENSSL)\srt.props" />
```

Header includes remain `<srt/srt.h>`; the root is private to this SDK and does
not install over a system Haivision installation. For OpenSSL packages, Crypto is an explicit
static link dependency. Applications with another OpenSSL dependency must resolve
version/link compatibility; static linkage does not remove symbol conflicts.
BCrypt packages instead link the Windows `bcrypt.lib` system import library.

The candidate workflow also builds the public C consumer through
`consumer/consumer.vcxproj`, importing the staged `srt.props` for all six
combinations. It executes Win32/x64 consumers and deliberately checks that a
static-CRT profile is rejected. ARM64 remains compile/link-only on this runner.

The installers do not set or change the legacy `ROBOTWEAX_SRT` variable.
Existing projects may explicitly set that variable to the selected SDK root.
This does not change the CMake default backend (OpenSSL).

Upgrades currently require uninstalling the previous SDK of the same backend
first. Legacy, backend-neutral SDK candidates must also be uninstalled first.
The installer rejects an already registered same-backend SDK even when a
different destination is selected. Different backends can coexist in distinct
folders. CI tests both installation orders, rejects cross-backend overwrites,
and verifies that removing one SDK preserves the other's files and variable.
CI checks rejection, preservation of the existing props file, uninstall and
reinstallation into another path. This is not an automatic in-place upgrade test.
Do not install
over an older SDK with a different file inventory. Uninstall removes the
environment variable only when it still points to this installation.

References: [Inno Setup command-line options](https://jrsoftware.org/ishelp/topic_setupcmdline.htm),
[OpenSSL Windows build notes](https://github.com/openssl/openssl/blob/openssl-3.6.3/NOTES-WINDOWS.md).
