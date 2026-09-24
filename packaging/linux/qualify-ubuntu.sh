#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends build-essential cmake debhelper dpkg-dev libssl-dev pkgconf
mkdir -p /build /out
cd /build
tar -xf /source.tar.gz
mv srt-492a7d61390cbec86e44e177ec034f0f5d9a5cc3 robotweax-srt-0.2.5
cp -R /work/packaging/debian robotweax-srt-0.2.5/debian
cd robotweax-srt-0.2.5
dpkg-buildpackage -b -us -uc
cp /build/*.deb /out/

apt-get install -y /build/librobotweax-srt0.2_0.2.5-1_*.deb /build/librobotweax-srt-dev_0.2.5-1_*.deb
test "$(pkg-config --modversion robotweax-srt)" = 0.2.5
cmake -S /work/packaging/tests -B /build/standalone -DTEST_HAIVISION=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build /build/standalone
ctest --test-dir /build/standalone --output-on-failure

apt-get install -y --no-install-recommends libsrt-openssl-dev
test -n "$(pkg-config --modversion srt)"
ctest --test-dir /build/standalone --output-on-failure
c++ /work/packaging/tests/haivision.cpp -o /build/haivision $(pkg-config --cflags --libs srt) -lssl -lcrypto
/build/haivision

dpkg -r librobotweax-srt-dev librobotweax-srt0.2
/build/haivision
test -n "$(pkg-config --modversion srt)"
