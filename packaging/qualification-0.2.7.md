# Package qualification 0.2.7

Status: **candidate source verified; CI qualification pending; not published**.

Product source: `f254dd2c0fe084f7965238756b1d3b96665d592b`.
Archive SHA-256: `7ba68805f66bca1da4e0d509d33aedab239a4de57ba9affb4db1481d4205c908`.
Archive SHA-512: `a22179bf28dbb4f681ddc44e22afc2ea0ca0e3e09d4652bb189fdb52574f75bb84f9c6a95e03549b41318043589c0bdb28c793241dff1de82b965446da952e0b`.

All 547 regular files and executable bits in the downloaded GitHub archive were
compared with the immutable Git source. These hashes describe those downloaded
bytes, not a local git archive. The product source remains separate from this
recipe/evidence commit. Changes to product files require a new source pin and
repeat qualification. No prior 0.2.6 result qualifies this candidate.

## Required acceptance

- Homebrew source install, test/audit, standalone consumers, coexistence and removal.
- Six vcpkg profiles: Linux x64, Windows x64 and macOS arm64, static/dynamic;
  Debug/Release consumers, relocation, coexistence and removal.
- Ubuntu 24.04 and Fedora 44 binary/source package builds, archive and license
  checks, installed consumers, coexistence and removal.
- Exact-head source CI and the explicit complete release ecosystem suite.
- Separately qualified final main/tag CI and signed Windows SDK download bytes.
- Separately qualified Homebrew tap formula and native bottle before advertising availability.

Record recipe SHA and exact successful run URLs before changing this status.
Package workflows build the pinned archive, not the moving workflow checkout.
Clean install/coexistence/removal checks do not establish upgrade/rollback,
PPA/COPR publication, registry availability or all-platform bottle coverage.
[Historical 0.2.6 qualification](qualification-0.2.6.md) remains unchanged.
