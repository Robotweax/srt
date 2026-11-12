#!/usr/bin/env bash
set -euo pipefail
echo "6769aaeffdc0cb8826c365a12e185ddd82e6af5c3d856e4de8236e980b3bcf3b  /source.tar.gz" | sha256sum --check -

dnf install -y rpm-build cmake gcc-c++ openssl-devel pkgconf-pkg-config cpio
mkdir -p /build/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS} /out
cp /source.tar.gz /build/rpmbuild/SOURCES/4a1e49166bd761d7f065bc5a4b9c924ca73333ea.tar.gz
rpmbuild --define '_topdir /build/rpmbuild' -bs /work/packaging/fedora/robotweax-srt.spec
test "$(rpm -qp --qf '%{LICENSE}' /build/rpmbuild/SRPMS/*.src.rpm)" = MIT
mkdir -p /build/source-reextract
cd /build/source-reextract
rpm2cpio /build/rpmbuild/SRPMS/*.src.rpm | cpio -id
cmp 4a1e49166bd761d7f065bc5a4b9c924ca73333ea.tar.gz /source.tar.gz
cp /build/rpmbuild/SRPMS/*.src.rpm /out/
rpmbuild --define '_topdir /build/rpmbuild' -bb /work/packaging/fedora/robotweax-srt.spec
cp /build/rpmbuild/RPMS/*/*.rpm /out/

dnf install -y /out/robotweax-srt-0.3.3-1*.rpm /out/robotweax-srt-devel-0.3.3-1*.rpm
test "$(pkg-config --modversion robotweax-srt)" = 0.3.3
cmake -S /work/packaging/tests -B /build/standalone -DTEST_HAIVISION=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build /build/standalone
ctest --test-dir /build/standalone --output-on-failure

dnf install -y srt-devel
test -n "$(pkg-config --modversion srt)"
ctest --test-dir /build/standalone --output-on-failure
c++ /work/packaging/tests/haivision.cpp -o /build/haivision $(pkg-config --cflags --libs srt) -lssl -lcrypto
/build/haivision

dnf remove -y robotweax-srt-devel robotweax-srt
/build/haivision
test -n "$(pkg-config --modversion srt)"
