# Package qualification: 0.2.6 candidate

Status: **qualification pending; no 0.2.6 package publication claimed**.
Source: `30505346cc6bb935abf68cab806b69e73d428bc1`.
Archive SHA-256: `feb7b2452b2b210417b3918d7ac13d4301f181fb2d465e54b8e87808712eb3b8`.
Archive SHA-512: `76e7d9761c254b4bb45684a3c48ec210a9bede1e186e7a79b584508343d31498919dec0f5fefa917c7ad9e34e21c6124a30e64ab6ab15d43557b2645e6cbf27c`.

The source commit contains the complete 0.2.6 runtime/storage changes. Later
recipe-only commits select that immutable archive; package workflows do not
build the moving branch. If implementation code changes during qualification,
update every recipe and hash to the new source commit and repeat affected gates.

## Required checks

- Homebrew source install, formula test/audit, standalone/coexistence/removal.
- vcpkg static and dynamic profiles on Windows x64, Linux x64 and macOS arm64;
  Debug/Release consumers, coexistence, relocation and removal.
- Ubuntu 24.04 DEB and Fedora 44 RPM builds, source archive/license checks,
  installed consumers, coexistence and removal.
- Separately published Homebrew tap update and native bottle qualification
  before advertising 0.2.6 bottle availability.

Record CI run links, exact tested commits and failures before changing this
status. Previous [0.2.5 evidence](qualification-0.2.5.md) is historical and does
not establish acceptance of this candidate. Upgrade paths, additional bottle
platforms, PPA/COPR publication and distribution admission remain outside the
existing clean-install package guarantees.
