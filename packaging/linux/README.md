# Ubuntu and Fedora package prototypes

These distribution recipes build the immutable Robotweax SRT 0.2.5 source
revision `492a7d61390cbec86e44e177ec034f0f5d9a5cc3`, not the repository
checkout. The source archive SHA-256 is
`dc55b9e1c2583eec6ff5ab82db70de6bfb0cc9c6e18cfd1ed15376f61d8fca5f`.
The CI workflow verifies this hash before starting a container.

Ubuntu 24.04 produces `librobotweax-srt0.2` and
`librobotweax-srt-dev`. Fedora 44 produces `robotweax-srt` and
`robotweax-srt-devel`. Both install Robotweax-specific library, headers,
CMake target and pkg-config metadata; they leave the existing Haivision SRT
package in place. The qualification scripts build packages, install and run
Robotweax consumers before and after installing Haivision, then remove
Robotweax and rerun the Haivision consumer.

The recipes and CI checks are packaging prototypes. They are not an Ubuntu
PPA, Fedora COPR, official distribution packages or a supported upgrade path.
Before publication, review distribution policy, licenses, source-package
generation, supported architectures, upgrade behavior, signing and repository
ownership. In particular, a clean build/install test is not upgrade evidence.

To repeat the CI test on a Docker host, download the pinned archive, verify
the SHA-256 above, and mount it as `/source.tar.gz`, this repository as
`/work:ro`, and an empty output directory as `/out`:

```sh
docker run --rm -v "$PWD:/work:ro" -v "$PWD/source.tar.gz:/source.tar.gz:ro" \
  -v "$PWD/deb-out:/out" ubuntu:24.04 bash /work/packaging/linux/qualify-ubuntu.sh
docker run --rm -v "$PWD:/work:ro" -v "$PWD/source.tar.gz:/source.tar.gz:ro" \
  -v "$PWD/rpm-out:/out" fedora:44 bash /work/packaging/linux/qualify-fedora.sh
```
