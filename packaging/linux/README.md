# Ubuntu and Fedora package prototypes

These distribution recipes build the immutable Robotweax SRT 0.2.6 source
revision `daac593ffcb9bbddd25126a2bd97ddb607792fc2`, not the repository
checkout. The source archive SHA-256 is
`852a9d4ca9d9a73c7a87be78252c1d641a0c4ffba5b6062b253249d8595cded4`.
The CI workflow verifies this hash before starting a container.

Ubuntu 24.04 produces `librobotweax-srt0.2` and
`librobotweax-srt-dev`. Fedora 44 produces `robotweax-srt` and
`robotweax-srt-devel`. Both install Robotweax-specific library, headers,
CMake target and pkg-config metadata; they leave the existing Haivision SRT
package in place. The qualification scripts build packages, install and run
Robotweax consumers before and after installing Haivision, then remove
Robotweax and rerun the Haivision consumer.

The Ubuntu recipe targets the `noble` series. Its test also creates an
unsigned `robotweax-srt_0.2.6-1_source.changes`, `.dsc`, packaging diff
and the original tarball. Lintian checks the source package, and extraction
checks that the original tarball is byte-for-byte the pinned upstream archive.
The Fedora test creates an SRPM, checks its MIT license metadata and extracts
the embedded source archive for the same comparison. CI retains these source
artifacts for review; its unsigned Ubuntu upload cannot be sent to Launchpad.

The recipes and CI checks are packaging prototypes. They are not an Ubuntu
PPA, Fedora COPR, official distribution packages or a supported upgrade path.
Before publication, review distribution policy, supported architectures,
upgrade behavior, signing and repository ownership. In particular, a clean
build/install test is not upgrade evidence. Launchpad needs a source `.changes`
and `.dsc` signed with an upload key associated with the PPA owner; `dput`
sends the signed upload to `ppa:<owner>/<archive>`. COPR accepts an SRPM through
its web UI or `copr-cli build <owner>/<project> <source-rpm>` after its
owner has configured a project and the desired chroots. Neither upload is
performed by CI, and no package repository credentials belong in this tree.

To repeat the CI test on a Docker host, download the pinned archive, verify
the SHA-256 above, and mount it as `/source.tar.gz`, this repository as
`/work:ro`, and an empty output directory as `/out`:

```sh
docker run --rm -v "$PWD:/work:ro" -v "$PWD/source.tar.gz:/source.tar.gz:ro" \
  -v "$PWD/deb-out:/out" ubuntu:24.04 bash /work/packaging/linux/qualify-ubuntu.sh
docker run --rm -v "$PWD:/work:ro" -v "$PWD/source.tar.gz:/source.tar.gz:ro" \
  -v "$PWD/rpm-out:/out" fedora:44 bash /work/packaging/linux/qualify-fedora.sh
```
