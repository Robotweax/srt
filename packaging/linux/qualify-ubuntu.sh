#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
echo "16753663e52f77e9795b462927147b1d47ac48719d7a4d7fb10cd1db28f80c1b  /source.tar.gz" | sha256sum --check -

apt-get update
apt-get install -y --no-install-recommends build-essential cmake debhelper dpkg-dev lintian libssl-dev pkgconf
mkdir -p /build /out
cd /build
cp /source.tar.gz robotweax-srt_0.2.9.orig.tar.gz
tar -xf /source.tar.gz
mv srt-80873657469344becb9c35f5306d0050ec5a566d robotweax-srt-0.2.9
cp -R /work/packaging/debian robotweax-srt-0.2.9/debian
cd robotweax-srt-0.2.9
dpkg-buildpackage -S -us -uc
cd /build
dpkg-source -x robotweax-srt_0.2.9-1.dsc /build/source-reextract
cmp robotweax-srt_0.2.9.orig.tar.gz /source.tar.gz
lintian --tag-display-limit 0 robotweax-srt_0.2.9-1.dsc
cp /build/*.dsc /build/*_source.changes /build/*_source.buildinfo /build/*.debian.tar.xz /build/*.orig.tar.gz /out/
cd robotweax-srt-0.2.9
dpkg-buildpackage -b -us -uc
cp /build/*.deb /out/

apt-get install -y /build/librobotweax-srt0.2_0.2.9-1_*.deb /build/librobotweax-srt-dev_0.2.9-1_*.deb
test "$(pkg-config --modversion robotweax-srt)" = 0.2.9
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
