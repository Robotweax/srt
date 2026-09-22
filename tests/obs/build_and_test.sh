#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 5 ]]; then
    echo "usage: $0 OBS_SOURCE GST_SOURCE FFMPEG_SOURCE REFERENCE_SRT_SOURCE WORK_DIRECTORY" >&2
    exit 2
fi
repository="$(cd "$(dirname "$0")/../.." && pwd -P)"
obs_source="$(cd "$1" && pwd -P)"
gst_source="$(cd "$2" && pwd -P)"
ffmpeg_source="$(cd "$3" && pwd -P)"
reference_source="$(cd "$4" && pwd -P)"
mkdir -p "$5"
work="$(cd "$5" && pwd -P)"
jobs="${JOBS:-2}"
[[ "$(git -C "$obs_source" rev-parse HEAD)" == ba2f32bdf791005443988a4955e963663e16b1ed ]]
[[ "$(git -C "$gst_source" rev-parse HEAD)" == 070125524a8422e29d3b69a372ed4f62fd343ffa ]]
[[ "$(git -C "$ffmpeg_source" rev-parse HEAD)" == 3acec0a1af2dda0a0838689b8b8649e7deb080a0 ]]
[[ "$(git -C "$reference_source" rev-parse HEAD)" == 899348d8318eb9a3c5a5b6ec43c4a1114288773a ]]
for directory in srt-build ffmpeg-build obs-build reference-build gst-build evidence robotweax-srt ffmpeg obs reference-srt gstreamer; do
    [[ ! -e "$work/$directory" ]] || { echo "use a fresh work directory" >&2; exit 1; }
done
mkdir -p "$work/evidence"
exec > >(tee "$work/evidence/build.log") 2>&1
cmake -S "$repository" -B "$work/srt-build" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
    -DROBOTWEAX_SRT_INSTALL_LAYOUT=namespaced \
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_PREFIX="$work/robotweax-srt" \
    -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF -DROBOTWEAX_SRT_BUILD_TOOLS=OFF \
    -DROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=ON
cmake --build "$work/srt-build" --parallel "$jobs"
ctest --test-dir "$work/srt-build" -L package --output-on-failure --timeout 120
cmake --install "$work/srt-build"
"$repository/tests/obs/configure_ffmpeg.sh" "$ffmpeg_source" "$work/ffmpeg-build" \
    "$work/robotweax-srt" "$work/ffmpeg"
# OBS's version finder needs a numeric prefix, even for a Git snapshot.
# Keep the source RELEASE baseline and immutable revision, never claim a
# released FFmpeg version. FFmpeg's Makefile supports this version input.
make -C "$work/ffmpeg-build" REVISION="$(<"$ffmpeg_source/RELEASE")-3acec0a" -j"$jobs"
make -C "$work/ffmpeg-build" REVISION="$(<"$ffmpeg_source/RELEASE")-3acec0a" install
"$repository/tests/obs/configure.sh" "$obs_source" "$work/obs-build" \
    "$work/robotweax-srt" "$work/ffmpeg" "$work/obs"
cmake --build "$work/obs-build" --parallel "$jobs"
cmake --install "$work/obs-build"
cmake -S "$reference_source" -B "$work/reference-build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_PREFIX="$work/reference-srt" \
    -DENABLE_SHARED=ON -DENABLE_STATIC=OFF \
    -DENABLE_APPS=OFF -DENABLE_TESTING=OFF -DUSE_ENCLIB=openssl
cmake --build "$work/reference-build" --parallel "$jobs"
cmake --install "$work/reference-build"
"$repository/tests/gstreamer/configure.sh" "$gst_source" "$work/gst-build" \
    "$work/reference-srt" "$work/gstreamer"
"${MESON:-meson}" compile -C "$work/gst-build" -j "$jobs"
"${MESON:-meson}" install -C "$work/gst-build" --no-rebuild
xvfb-run -a "$repository/tools/python" "$repository/tests/obs/run_smoke.py" \
    --obs-prefix "$work/obs" --srt-prefix "$work/robotweax-srt" \
    --ffmpeg-prefix "$work/ffmpeg" --gst-prefix "$work/gstreamer" \
    --reference-srt-prefix "$work/reference-srt" --artifacts "$work/evidence" \
    | tee "$work/evidence/results.txt"
