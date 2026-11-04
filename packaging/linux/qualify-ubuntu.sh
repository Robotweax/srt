#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
echo "709e3a353ca4cdf0afea6de2d363934c313117f1ccc8121a3b641d5155c1658a  /source.tar.gz" | sha256sum --check -

apt-get update
apt-get install -y --no-install-recommends build-essential cmake debhelper dpkg-dev lintian libssl-dev pkgconf
mkdir -p /build /out
cd /build
cp /source.tar.gz robotweax-srt_0.3.2.orig.tar.gz
tar -xf /source.tar.gz
mv srt-b19c6a88b6257c749e16dc1e53d803cc3c792e32 robotweax-srt-0.3.2
cp -R /work/packaging/debian robotweax-srt-0.3.2/debian
cd robotweax-srt-0.3.2
dpkg-buildpackage -S -us -uc
cd /build
dpkg-source -x robotweax-srt_0.3.2-1.dsc /build/source-reextract
cmp robotweax-srt_0.3.2.orig.tar.gz /source.tar.gz
lintian --tag-display-limit 0 robotweax-srt_0.3.2-1.dsc
cp /build/*.dsc /build/*_source.changes /build/*_source.buildinfo /build/*.debian.tar.xz /build/*.orig.tar.gz /out/
cd robotweax-srt-0.3.2
dpkg-buildpackage -b -us -uc
cp /build/*.deb /out/

apt-get install -y /build/librobotweax-srt0.3_0.3.2-1_*.deb /build/librobotweax-srt-dev_0.3.2-1_*.deb
test "$(pkg-config --modversion robotweax-srt)" = 0.3.2
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
