#!/usr/bin/env bash
set -euo pipefail

robotweax_source="${1:-/opt/robotweax-srt}"
reference_commit="899348d8318eb9a3c5a5b6ec43c4a1114288773a"
reference_source="${robotweax_source}/reference-srt"
robotweax_build="${robotweax_source}/build"
reference_build="${robotweax_source}/reference-build"

if [[ ! -f "${robotweax_source}/CMakeLists.txt" ]]; then
    echo "Robotweax source tree not found at ${robotweax_source}" >&2
    exit 2
fi

sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    ca-certificates \
    cmake \
    git \
    libssl-dev \
    ninja-build \
    pkg-config \
    python3 \
    tcpdump

python3 -c \
    "import sys; sys.version_info >= (3, 10) or sys.exit('Python 3.10 or newer is required')"

if [[ ! -d "${reference_source}/.git" ]]; then
    git clone https://github.com/Haivision/srt.git "${reference_source}"
fi
if ! git -C "${reference_source}" cat-file -e "${reference_commit}^{commit}"; then
    git -C "${reference_source}" fetch --quiet --tags origin
fi
git -C "${reference_source}" checkout --detach "${reference_commit}"
test "$(git -C "${reference_source}" rev-parse --verify HEAD)" = "${reference_commit}"
git -C "${reference_source}" diff --exit-code HEAD --

cmake -S "${robotweax_source}" -B "${robotweax_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
    -DROBOTWEAX_SRT_BUILD_FUZZERS=OFF
cmake --build "${robotweax_build}" --parallel

cmake -S "${reference_source}" -B "${reference_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_APPS=ON \
    -DENABLE_ENCRYPTION=ON \
    -DENABLE_TESTING=OFF
cmake --build "${reference_build}" --parallel

c++ -std=c++17 -O2 \
    "${robotweax_source}/interop/api_peer.cpp" \
    -I"${reference_source}/srtcore" \
    -I"${reference_build}" \
    "${reference_build}/libsrt.a" \
    -lcrypto -lpthread -ldl \
    -o "${reference_build}/haivision_srt_interop_peer"

test -x "${robotweax_build}/robotweax_srt_interop_peer"
test -x "${reference_build}/haivision_srt_interop_peer"

echo "Robotweax peer:  ${robotweax_build}/robotweax_srt_interop_peer"
echo "Haivision peer: ${reference_build}/haivision_srt_interop_peer"
echo "Reference commit: $(git -C "${reference_source}" rev-parse HEAD)"
