#!/usr/bin/env bash
set -euo pipefail
echo "4798208d99d66417209a461d4b24e4a6269b6ea740fbee8d8beab7e5b295258d  /source.tar.gz" | sha256sum --check -

dnf install -y rpm-build cmake gcc-c++ openssl-devel pkgconf-pkg-config cpio
mkdir -p /build/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS} /out
cp /source.tar.gz /build/rpmbuild/SOURCES/d6ad0c3e595b6ce5d518d3310e103e480fbc633d.tar.gz
rpmbuild --define '_topdir /build/rpmbuild' -bs /work/packaging/fedora/robotweax-srt.spec
test "$(rpm -qp --qf '%{LICENSE}' /build/rpmbuild/SRPMS/*.src.rpm)" = MIT
mkdir -p /build/source-reextract
cd /build/source-reextract
rpm2cpio /build/rpmbuild/SRPMS/*.src.rpm | cpio -id
cmp d6ad0c3e595b6ce5d518d3310e103e480fbc633d.tar.gz /source.tar.gz
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
