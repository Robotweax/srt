# Ubuntu and Fedora package prototypes

These distribution recipes build the immutable Robotweax SRT 0.3.4 source
revision `6009c6430b46e8691181bbd8690bf36c3ef0d661`, not the repository
checkout. The source archive SHA-256 is
`66ab41f65523fce7cea66c5ac54b44f3f8d7b5a87586755b00ee91a70d870116`.
The CI workflow verifies this hash before starting a container.

Ubuntu 26.04 produces `librobotweax-srt0.3` and
`librobotweax-srt-dev`. Fedora 44 produces `robotweax-srt` and
`robotweax-srt-devel`. Both install Robotweax-specific library, headers,
CMake target and pkg-config metadata; they leave the existing Haivision SRT
package in place. The qualification scripts build packages, install and run
Robotweax consumers before and after installing Haivision, then remove
Robotweax and rerun the Haivision consumer.

The Ubuntu recipe targets the `resolute` series. Its test also creates an
unsigned `robotweax-srt_0.3.4-1_source.changes`, `.dsc`, packaging diff
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
  -v "$PWD/deb-out:/out" ubuntu:26.04 bash /work/packaging/linux/qualify-ubuntu.sh
docker run --rm -v "$PWD:/work:ro" -v "$PWD/source.tar.gz:/source.tar.gz:ro" \
  -v "$PWD/rpm-out:/out" fedora:44 bash /work/packaging/linux/qualify-fedora.sh
```
