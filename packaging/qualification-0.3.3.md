# Package inputs and qualification — 0.3.3

Qualification is in progress. This record does not authorize publication.
Project version is 0.3.3, ABI is 0.3, compatible API is 1.5.7; all 85 C exports
from 0.3.2 are preserved. Historical qualification records remain unchanged.

## Immutable product

- Product commit: `d1f1b15393c0d0b5598c0af308b24966cbbfbd5e`.
- [Public archive](https://github.com/Robotweax/srt/archive/d1f1b15393c0d0b5598c0af308b24966cbbfbd5e.tar.gz).
- Bytes: 3150219.
- SHA-256: `0bf14b9b0de8f8a0a5688dfd1ce6e5e4cf8bdb7cb0f8abd466e1d895cb6d6a99`.
- SHA-512: `7beb067c9e00be98c9e2722d83a7607a1960c1d88faaf7e992e724857c7823f2497148bc545a8e32288f8a3d01f9d413846d21f9b89df53a03f794e03175fed2`.
- All 944 regular files and executable bits matched Git.
- [Machine-readable identity and frozen paths](source-0.3.3.json).

Recipe/evidence commits must preserve the frozen paths. Product changes require
another archive and qualification. See the [release contract](../docs/release-process-0.3.3.md).

## Preparation evidence

The Python harness passed 868 tests. Changed C/C++ formatting passed with
clang-format 22.1.8. The versioned native Release build succeeded; its complete
runtime suite and hosted release gates are pending. These preparation checks
alone do not establish release acceptance.

Previous signed Windows upgrade inputs are 0.3.2, with SHA-256:

- OpenSSL: `ad1998a27cd6e6136111ab024eb450cc666bc7911f2284fd9aae4ed1fbb5aff5`.
- BCrypt: `5b1af8638c0bc04622a9cd185e4e10ebf9739f977d4e375f26f15771497f0fa9`.

## Pending gates

Full source CI and ecosystem; all provider, multicast, ACP1, hybrid and sanitizer
matrices; Windows timer OFF; installed docs/consumers; commit-bound hybrid soak;
Homebrew, six vcpkg profiles, DEB/RPM; exact-source Linux/downstream qualification;
protected annotated tag, independent Windows rebuild/signing and final assets.
