#!/usr/bin/env bash
set -euo pipefail
echo "b7f0cb906abe8d064e7d7c18287596ff0f594ac6cb07aaa10f5c791971683a9b  /source.tar.gz" | sha256sum --check -

dnf install -y rpm-build cmake gcc-c++ openssl-devel pkgconf-pkg-config cpio
mkdir -p /build/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS} /out
cp /source.tar.gz /build/rpmbuild/SOURCES/838a022b75cfa95137bb7e40f575d0a65f6bd551.tar.gz
rpmbuild --define '_topdir /build/rpmbuild' -bs /work/packaging/fedora/robotweax-srt.spec
test "$(rpm -qp --qf '%{LICENSE}' /build/rpmbuild/SRPMS/*.src.rpm)" = MIT
mkdir -p /build/source-reextract
cd /build/source-reextract
rpm2cpio /build/rpmbuild/SRPMS/*.src.rpm | cpio -id
cmp 838a022b75cfa95137bb7e40f575d0a65f6bd551.tar.gz /source.tar.gz
cp /build/rpmbuild/SRPMS/*.src.rpm /out/
rpmbuild --define '_topdir /build/rpmbuild' -bb /work/packaging/fedora/robotweax-srt.spec
cp /build/rpmbuild/RPMS/*/*.rpm /out/

dnf install -y /out/robotweax-srt-0.3.2-1*.rpm /out/robotweax-srt-devel-0.3.2-1*.rpm
test "$(pkg-config --modversion robotweax-srt)" = 0.3.2
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
