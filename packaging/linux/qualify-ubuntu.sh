#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
echo "6c6d19458838248a6c95153e8907334e6c16826c041d4e204f57b73a1750cb5c  /source.tar.gz" | sha256sum --check -

apt-get update
apt-get install -y --no-install-recommends build-essential cmake debhelper dpkg-dev lintian libssl-dev pkgconf
mkdir -p /build /out
cd /build
cp /source.tar.gz robotweax-srt_0.3.0.orig.tar.gz
tar -xf /source.tar.gz
mv srt-607d0034b7977f1e5003c98c42cc934bbdf9d431 robotweax-srt-0.3.0
cp -R /work/packaging/debian robotweax-srt-0.3.0/debian
cd robotweax-srt-0.3.0
dpkg-buildpackage -S -us -uc
cd /build
dpkg-source -x robotweax-srt_0.3.0-1.dsc /build/source-reextract
cmp robotweax-srt_0.3.0.orig.tar.gz /source.tar.gz
lintian --tag-display-limit 0 robotweax-srt_0.3.0-1.dsc
cp /build/*.dsc /build/*_source.changes /build/*_source.buildinfo /build/*.debian.tar.xz /build/*.orig.tar.gz /out/
cd robotweax-srt-0.3.0
dpkg-buildpackage -b -us -uc
cp /build/*.deb /out/

apt-get install -y /build/librobotweax-srt0.3_0.3.0-1_*.deb /build/librobotweax-srt-dev_0.3.0-1_*.deb
test "$(pkg-config --modversion robotweax-srt)" = 0.3.0
cmake -S /work/packaging/tests -B /build/standalone -DTEST_HAIVISION=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build /build/standalone
ctest --test-dir /build/standalone --output-on-failure

apt-get install -y --no-install-recommends libsrt-openssl-dev
test -n "$(pkg-config --modversion srt)"
ctest --test-dir /build/standalone --output-on-failure
c++ /work/packaging/tests/haivision.cpp -o /build/haivision $(pkg-config --cflags --libs srt) -lssl -lcrypto
/build/haivision

dpkg -r librobotweax-srt-dev librobotweax-srt0.3
/build/haivision
test -n "$(pkg-config --modversion srt)"
