#!/usr/bin/env bash
set -euo pipefail
echo "7ba68805f66bca1da4e0d509d33aedab239a4de57ba9affb4db1481d4205c908  /source.tar.gz" | sha256sum --check -

dnf install -y rpm-build cmake gcc-c++ openssl-devel pkgconf-pkg-config cpio
mkdir -p /build/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS} /out
cp /source.tar.gz /build/rpmbuild/SOURCES/f254dd2c0fe084f7965238756b1d3b96665d592b.tar.gz
rpmbuild --define '_topdir /build/rpmbuild' -bs /work/packaging/fedora/robotweax-srt.spec
test "$(rpm -qp --qf '%{LICENSE}' /build/rpmbuild/SRPMS/*.src.rpm)" = MIT
mkdir -p /build/source-reextract
cd /build/source-reextract
rpm2cpio /build/rpmbuild/SRPMS/*.src.rpm | cpio -id
cmp f254dd2c0fe084f7965238756b1d3b96665d592b.tar.gz /source.tar.gz
cp /build/rpmbuild/SRPMS/*.src.rpm /out/
rpmbuild --define '_topdir /build/rpmbuild' -bb /work/packaging/fedora/robotweax-srt.spec
cp /build/rpmbuild/RPMS/*/*.rpm /out/

dnf install -y /out/robotweax-srt-0.2.7-1*.rpm /out/robotweax-srt-devel-0.2.7-1*.rpm
test "$(pkg-config --modversion robotweax-srt)" = 0.2.7
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
