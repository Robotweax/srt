#!/usr/bin/env bash
set -euo pipefail

dnf install -y rpm-build cmake gcc-c++ openssl-devel pkgconf-pkg-config
mkdir -p /build/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS} /out
cp /source.tar.gz /build/rpmbuild/SOURCES/492a7d61390cbec86e44e177ec034f0f5d9a5cc3.tar.gz
rpmbuild --define '_topdir /build/rpmbuild' -bb /work/packaging/fedora/robotweax-srt.spec
cp /build/rpmbuild/RPMS/*/*.rpm /out/

dnf install -y /out/robotweax-srt-0.2.5-1*.rpm /out/robotweax-srt-devel-0.2.5-1*.rpm
test "$(pkg-config --modversion robotweax-srt)" = 0.2.5
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
