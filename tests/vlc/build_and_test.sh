#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 ]]; then
    echo "usage: $0 VLC_SOURCE GST_SOURCE FFMPEG_SOURCE REFERENCE_SRT_SOURCE WORK_DIRECTORY" >&2
    exit 2
fi
repository="$(cd "$(dirname "$0")/../.." && pwd -P)"
vlc_source="$(cd "$1" && pwd -P)"
gst_source="$(cd "$2" && pwd -P)"
ffmpeg_source="$(cd "$3" && pwd -P)"
reference_source="$(cd "$4" && pwd -P)"
"$repository/tools/python" "$repository/tests/check_source_revision.py" "$vlc_source" 6de05adcbaf2e8b85fe86aad4169393098628119
"$repository/tools/python" "$repository/tests/check_source_revision.py" "$gst_source" 070125524a8422e29d3b69a372ed4f62fd343ffa
"$repository/tools/python" "$repository/tests/check_source_revision.py" "$ffmpeg_source" 3acec0a1af2dda0a0838689b8b8649e7deb080a0
"$repository/tools/python" "$repository/tests/check_source_revision.py" "$reference_source" 899348d8318eb9a3c5a5b6ec43c4a1114288773a
mkdir -p "$5"
work="$(cd "$5" && pwd -P)"
jobs="${JOBS:-2}"
meson="${MESON:-meson}"

cmake -S "$repository" -B "$work/srt-build" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
    -DROBOTWEAX_SRT_INSTALL_LAYOUT=namespaced \
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_PREFIX="$work/robotweax-srt" \
    -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF -DROBOTWEAX_SRT_BUILD_TOOLS=OFF \
    -DROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=ON
cmake --build "$work/srt-build" --parallel "$jobs"
ctest --test-dir "$work/srt-build" -L package --output-on-failure --timeout 120
cmake --install "$work/srt-build"

"$repository/tests/vlc/configure.sh" "$vlc_source" "$work/vlc-build" \
    "$work/robotweax-srt" "$work/vlc"
make -C "$work/vlc-build" -j"$jobs"
make -C "$work/vlc-build" install

cmake -S "$reference_source" -B "$work/reference-build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_PREFIX="$work/reference-srt" \
    -DENABLE_SHARED=ON -DENABLE_STATIC=OFF \
    -DENABLE_APPS=OFF -DENABLE_TESTING=OFF -DUSE_ENCLIB=openssl
cmake --build "$work/reference-build" --parallel "$jobs"
cmake --install "$work/reference-build"
"$repository/tests/gstreamer/configure.sh" "$gst_source" "$work/gst-build" \
    "$work/reference-srt" "$work/gstreamer"
"$meson" compile -C "$work/gst-build" -j "$jobs"
"$meson" install -C "$work/gst-build" --no-rebuild

"$repository/tests/ffmpeg/configure.sh" "$ffmpeg_source" "$work/robotweax-srt"
make -C "$ffmpeg_source" -j"$jobs" ffmpeg
"$repository/tools/python" "$repository/tests/vlc/run_smoke.py" \
    --vlc-prefix "$work/vlc" --srt-prefix "$work/robotweax-srt" \
    --gst-prefix "$work/gstreamer" --reference-srt-prefix "$work/reference-srt" \
    --ffmpeg "$ffmpeg_source/ffmpeg" --artifacts "$work/evidence"
