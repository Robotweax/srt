# OBS Windows portable preview

This optional, user-invoked profile assembles the
[qualified Windows desktop build](obs-windows-desktop.md) as an isolated,
portable OBS Studio 32.2.2 x64 ZIP. The Robotweax CI does **not** invoke the
`-Preview` build mode against the real OBS runtime or publish a binary
artifact. A synthetic unit test checks the packager with fixture files, but
does not produce a usable OBS distribution. This is a development preview,
not a signed installer or an official OBS Project distribution. It does not
replace an existing OBS installation.

## Build and inspect

Use the prerequisites and pinned OBS checkout from the
[Windows integration guide](obs-windows.md). Start in an x64 Visual Studio C++
developer environment, with CMake and Python 3.10 or newer. Use a fresh work
directory:

```powershell
./tests/obs/build_windows.ps1 `
  -ObsSource C:/src/obs-studio-desktop `
  -WorkDirectory C:/build/robotweax-obs-preview `
  -Desktop -Preview
```

The strict Windows module and real Qt desktop SRT smoke tests must pass before
the ZIP is made. The resulting files are in `C:/build/robotweax-obs-preview/preview/`:

- `Robotweax-OBS-32.2.2-Windows-x64-preview.zip` — portable runtime and bundled
  license notices.
- `MANIFEST.json` — per-file SHA-256 hashes, selected Robotweax source commit,
  pinned OBS commit, dependency release, and selected SRT DLL hash.
- `SHA256SUMS.txt` — SHA-256 of the ZIP.

Check the ZIP hash before extraction:

```powershell
$preview = 'C:/build/robotweax-obs-preview/preview'
$zip = Join-Path $preview 'Robotweax-OBS-32.2.2-Windows-x64-preview.zip'
Get-FileHash -Algorithm SHA256 $zip
Get-Content (Join-Path $preview 'SHA256SUMS.txt')
Expand-Archive $zip -DestinationPath C:/test/robotweax-obs-preview
& C:/test/robotweax-obs-preview/Robotweax-OBS-32.2.2-Windows-x64-preview/bin/64bit/obs64.exe
```

The hash from `Get-FileHash` must match `SHA256SUMS.txt`. The ZIP contains
`portable_mode.txt` at its root; OBS keeps its configuration in that extracted
tree. Do not copy its files into an existing OBS installation. The packager
rejects a non-32.2.2 pinned OBS source, absent license/runtime inputs, an
incorrect Robotweax DLL, or a second `srt.dll`. It excludes test peers, build
symbols and the test's private `config` directory. The ZIP is deterministic
for identical inputs, and its embedded manifest is verified after writing.

## Physical Windows acceptance

The CI desktop smoke runs on a hosted Windows runner; it does not establish
hardware or production readiness. On a physical Windows x64 machine, extract
the ZIP into a fresh directory and verify:

1. OBS opens with a visible Qt window and does not alter an existing OBS
   installation or profile.
2. A moving-video and audible-stereo-audio source can be sent via OBS's native
   encrypted SRT output to an independent receiver. Inspect decoded picture
   and sound, not merely connection status.
3. Interrupt the receiver and confirm OBS reconnects without restarting the
   output. Repeat the decoded video/audio check after reconnection.
4. Receive an independently encrypted SRT feed via OBS's FFmpeg media source,
   then forward it through a separate encrypted native SRT output. Inspect
   decoded video and audio at the far end.
5. Close OBS normally and confirm the process exits. Record OBS logs, Windows
   version, graphics driver, CPU/GPU, and observed behavior.

Do not reuse the development test's passphrase or publish capture content
containing secrets. Hardware encoders, capture devices, adverse networks,
AES-GCM, quantitative A/V sync and long-duration operation remain outside
this qualification.

## Distribution boundary

GitHub Actions runs the desktop qualification without `-Preview` and uploads
text diagnostics only. It neither creates nor distributes the OBS runtime ZIP.
Users who invoke the optional packaging switch are responsible for the
resulting local artifact and any distribution. The ZIP includes collected
OBS, Robotweax and dependency license files, but that alone does not
establish compliance with every source-offer, patch, attribution, patent or
redistribution obligation. Before sharing the ZIP with others, review the
exact pinned OBS/dependency licenses and make the corresponding modified
source and build scripts available as required. The build's
OBS modifications are applied by `tests/obs/prepare_source.py`,
`tests/obs/prepare_desktop_lifecycle.py`, and
`tests/obs/prepare_windows_source.py` in the Robotweax source commit recorded
in `MANIFEST.json`.
