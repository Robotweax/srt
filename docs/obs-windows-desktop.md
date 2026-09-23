# OBS Windows desktop qualification

This profile extends the [Windows module integration](obs-windows.md) to the
installed OBS Studio 32.2.2 Qt frontend on Windows x64. It uses the same pinned
OBS revision and Robotweax BCrypt compatibility DLL, but a **separate clean
OBS checkout and fresh work directory**. It does not install or replace a
system-wide OBS distribution.

## Build and run

Use an x64 Visual Studio C++ developer environment with CMake and Python 3.10
or newer. From the Robotweax repository, with OBS checked out at the revision
listed in the Windows module guide:

```powershell
./tests/obs/build_windows.ps1 `
  -ObsSource C:/src/obs-studio-desktop `
  -WorkDirectory C:/build/robotweax-obs-windows-desktop `
  -Desktop
```

The script builds Robotweax and the full OBS Qt frontend into an isolated
prefix. Its hash-guarded source preparation selects `obs-ffmpeg`, `obs-x264`,
`rtmp-services` and `obs-transitions`, and applies the pinned MPEG-TS lifecycle
cleanup described in the [Linux desktop guide](obs-desktop.md). The browser,
scripting, virtual camera, updater dialog and service-list updates are not part
of this profile. OBS's own build downloads its pinned Windows and Qt dependency
archives. The installed `bin/64bit/srt.dll` must match the Robotweax build;
the reference provider is kept in a separate directory and process.

The unchanged strict Windows module smoke runs first against this desktop
build. The desktop test then uses a private `config` directory inside the new
prefix and starts the **installed `obs64.exe` in portable mode**. It requires a
visible Qt window, sends a local moving-video/stereo-audio fixture through
OBS's native encrypted SRT output to the separate reference listener, and
decodes the received MPEG-TS with FFmpeg. After the listener is interrupted
for eight seconds, OBS must automatically reconnect to a new listener on the
same port without another start command. Both captures must contain moving
decoded video and audible audio. The private test profile disables OBS's
interactive exit confirmation so the active stream can shut down normally in
unattended CI. Normal frontend shutdown and its allocation
count are checked. A second GUI launch uses a separately encrypted SRT
listener as its FFmpeg media source, while OBS forwards the decoded picture
and sound through a different encrypted native SRT output. The reference
sender remains active until the round-trip capture proves moving video and
audible audio. Results and diagnostic logs are in the fresh work
directory's `evidence` folder; CI uploads text diagnostics only.

Use [`-Desktop -Preview`](obs-windows-preview.md) to assemble a separate
portable ZIP after the same strict qualification. The ZIP is a local
development output; CI publishes only its text manifest and hash.

This is a reproducible development qualification, **not** a signed installer,
a release package, or proof for every Windows desktop workflow. It does not
qualify ARM64, hardware encoders, device/screen capture, adverse networks,
AES-GCM, quantitative A/V synchronization, or long-duration operation.
