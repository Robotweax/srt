#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
echo "b2920453b222879e1c7181a7c6b895c9440d104987d01cf9699eb4126b3b2476  /source.tar.gz" | sha256sum --check -

apt-get update
apt-get install -y --no-install-recommends build-essential cmake debhelper dpkg-dev lintian libssl-dev pkgconf
mkdir -p /build /out
cd /build
cp /source.tar.gz robotweax-srt_0.2.6.orig.tar.gz
tar -xf /source.tar.gz
mv srt-7ecb60ea8b4faca01ed86237b0cc9dc906350f6e robotweax-srt-0.2.6
cp -R /work/packaging/debian robotweax-srt-0.2.6/debian
cd robotweax-srt-0.2.6
dpkg-buildpackage -S -us -uc
cd /build
dpkg-source -x robotweax-srt_0.2.6-1.dsc /build/source-reextract
cmp robotweax-srt_0.2.6.orig.tar.gz /source.tar.gz
lintian --tag-display-limit 0 robotweax-srt_0.2.6-1.dsc
cp /build/*.dsc /build/*_source.changes /build/*_source.buildinfo /build/*.debian.tar.xz /build/*.orig.tar.gz /out/
cd robotweax-srt-0.2.6
dpkg-buildpackage -b -us -uc
cp /build/*.deb /out/

apt-get install -y /build/librobotweax-srt0.2_0.2.6-1_*.deb /build/librobotweax-srt-dev_0.2.6-1_*.deb
test "$(pkg-config --modversion robotweax-srt)" = 0.2.6
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
