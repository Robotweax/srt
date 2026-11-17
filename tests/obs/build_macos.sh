#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

if [[ $# -lt 3 || $# -gt 4 ]]; then
    echo "usage: $0 OBS_SOURCE FFMPEG_SOURCE WORK_DIRECTORY [modules|desktop]" >&2
    exit 2
fi
profile="${4:-modules}"
case "$profile" in
    modules) frontend=OFF; selection=headless; desktop_cmake=(-DROBOTWEAX_OBS_MACOS_DESKTOP=OFF) ;;
    desktop) frontend=ON; selection=desktop; desktop_cmake=(-DROBOTWEAX_OBS_MACOS_DESKTOP=ON -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0) ;;
    *) echo "unknown OBS profile: $profile" >&2; exit 2 ;;
esac
repository="$(cd "$(dirname "$0")/../.." && pwd -P)"
obs_source="$(cd "$1" && pwd -P)"
ffmpeg_source="$(cd "$2" && pwd -P)"
mkdir -p "$3"
work="$(cd "$3" && pwd -P)"
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]]
[[ "$(git -C "$obs_source" rev-parse HEAD)" == ba2f32bdf791005443988a4955e963663e16b1ed ]]
[[ "$(git -C "$ffmpeg_source" rev-parse HEAD)" == 3acec0a1af2dda0a0838689b8b8649e7deb080a0 ]]
source_exclusions=(':!plugins/CMakeLists.txt')
if [[ "$profile" == desktop ]]; then
    source_exclusions+=(':!plugins/obs-ffmpeg/obs-ffmpeg-mpegts.c')
    source_exclusions+=(':!libobs/util/bmem.c' ':!frontend/OBSApp.cpp' ':!frontend/widgets/OBSBasic_Hotkeys.cpp')
fi
[[ -z "$(git -C "$obs_source" status --porcelain --untracked-files=no -- . "${source_exclusions[@]}")" ]]
for directory in srt-build srt ffmpeg-build ffmpeg obs-build deps evidence; do
    [[ ! -e "$work/$directory" ]] || { echo "use a fresh work directory" >&2; exit 1; }
done
mkdir -p "$work/evidence" "$work/deps"
exec > >(tee "$work/evidence/build.log") 2>&1
jobs="${JOBS:-3}"

# OBS 32.2.2 pins this archive and hash in CMakePresets.json. It is used
# only for independent OBS libraries/x264 and the reference peer. FFmpeg is
# built against the namespaced Robotweax dylib below, never this archive's SRT.
archive="$work/deps/macos-deps-2026-07-15-universal.tar.xz"
curl --fail --location --retry 3 --output "$archive" \
    'https://github.com/obsproject/obs-deps/releases/download/2026-07-15/macos-deps-2026-07-15-universal.tar.xz'
echo '4ecb4c598dfa853168df6c2a0c4e0ffec8495a81fbd1ba051ef88ecd5e0f7e53  '"$archive" | shasum -a 256 -c -
mkdir -p "$work/deps/obs"
tar -xf "$archive" -C "$work/deps/obs"
[[ -f "$work/deps/obs/include/srt/srt.h" ]]
[[ -f "$work/deps/obs/lib/libmbedcrypto.dylib" ]]
prefix_path="$work/ffmpeg;$work/srt;$work/deps/obs"
cache_check=()
if [[ "$profile" == desktop ]]; then
    # The Qt 6.11.1 archive pinned by OBS 32.2.2 (obs-deps 2026-07-15) releases
    # the color space of an ICC-tagged QImage before CGImageCreate reads it
    # (QTBUG-147602). macOS 26.7 no longer keeps that color space alive, so OBS
    # traps on its first wait cursor while loading the scene collection.
    # obs-deps 2026-10-23, pinned with this hash by OBS 33.0.1, is Qt 6.11.1
    # with the upstream fix backported. Only the desktop Qt is replaced; OBS,
    # its other dependencies and the tested SRT/FFmpeg stack stay pinned.
    qt_archive="$work/deps/macos-deps-qt6-2026-10-23-universal.tar.xz"
    curl --fail --location --retry 3 --output "$qt_archive" \
        'https://github.com/obsproject/obs-deps/releases/download/2026-10-23/macos-deps-qt6-2026-10-23-universal.tar.xz'
    echo 'b216560cea8497207f9fc350c713716fb84aec6b973bb0ff8ac6add7734abad5  '"$qt_archive" | shasum -a 256 -c -
    mkdir -p "$work/deps/qt6"
    tar -xf "$qt_archive" -C "$work/deps/qt6"
    [[ -f "$work/deps/qt6/lib/cmake/Qt6/Qt6Config.cmake" ]]
    # A caller-supplied prefix precedes the Qt that OBS downloads for itself.
    prefix_path="$work/ffmpeg;$work/srt;$work/deps/qt6;$work/deps/obs"
    cache_check=(--qt "$work/deps/qt6")
fi

openssl_prefix="$(brew --prefix openssl@3)"
cmake -S "$repository" -B "$work/srt-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
    -DOPENSSL_ROOT_DIR="$openssl_prefix" \
    -DROBOTWEAX_SRT_INSTALL_LAYOUT=namespaced \
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_PREFIX="$work/srt" \
    -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF -DROBOTWEAX_SRT_BUILD_TOOLS=OFF \
    -DROBOTWEAX_SRT_INSTALL_LIBSRT_PKGCONFIG_COMPAT=ON
cmake --build "$work/srt-build" --parallel "$jobs"
ctest --test-dir "$work/srt-build" -L package --output-on-failure --timeout 120
cmake --install "$work/srt-build"

export PKG_CONFIG_PATH="$work/srt/lib/pkgconfig"
[[ "$(pkg-config --variable=pcfiledir srt)" == "$work/srt/lib/pkgconfig" ]]
[[ "$(pkg-config --libs-only-l srt)" == *-lrobotweax-srt* ]]
mkdir -p "$work/ffmpeg-build"
(
    cd "$work/ffmpeg-build"
    "$ffmpeg_source/configure" --prefix="$work/ffmpeg" --libdir="$work/ffmpeg/lib" \
        --disable-autodetect --disable-debug --disable-doc --disable-static \
        --enable-shared --disable-everything --enable-libsrt --enable-zlib \
        --enable-ffmpeg --enable-ffprobe --enable-avdevice --enable-avfilter \
        --enable-swscale --enable-swresample \
        --enable-encoder=mpeg2video,aac,pcm_s16le,rawvideo,wrapped_avframe \
        --enable-decoder=h264,aac,mpeg2video,pcm_s16le,wrapped_avframe,png \
        --enable-parser=h264,aac,mpegvideo \
        --enable-demuxer=mpegts,image2,image2pipe \
        --enable-muxer=mpegts,framehash,pcm_s16le \
        --enable-protocol=file,pipe,libsrt \
        --enable-filter=testsrc2,sine,aresample,scale,format,aformat,anull,null \
        --enable-indev=lavfi \
        --extra-ldflags="-Wl,-rpath,$work/srt/lib -Wl,-rpath,$work/ffmpeg/lib"
)
make -C "$work/ffmpeg-build" REVISION="$(<"$ffmpeg_source/RELEASE")-3acec0a" -j"$jobs"
make -C "$work/ffmpeg-build" REVISION="$(<"$ffmpeg_source/RELEASE")-3acec0a" install

if [[ "$profile" == desktop ]]; then
    "$repository/tools/python" "$repository/tests/obs/prepare_desktop_lifecycle.py" "$obs_source"
    "$repository/tools/python" "$repository/tests/obs/prepare_desktop_trace.py" "$obs_source"
    "$repository/tools/python" "$repository/tests/obs/prepare_desktop_swapchain.py" "$obs_source"
fi
"$repository/tools/python" "$repository/tests/obs/prepare_source.py" "$obs_source" \
    --profile "$selection"
export PKG_CONFIG_PATH="$work/ffmpeg/lib/pkgconfig:$work/srt/lib/pkgconfig"
for component in libavcodec libavdevice libavfilter libavformat libavutil libswscale libswresample; do
    [[ "$(pkg-config --variable=pcfiledir "$component")" == "$work/ffmpeg/lib/pkgconfig" ]]
done
robotweax_dylib="$(find "$work/srt/lib" -maxdepth 1 -name 'librobotweax-srt*.dylib' -print | sort | head -1)"
[[ -n "$robotweax_dylib" ]]
cmake -S "$obs_source" -B "$work/obs-build" -G Xcode \
    -DOBS_VERSION_OVERRIDE=32.2.2-robotweax-macos-qualification \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_PREFIX_PATH="$prefix_path" \
    -DLibsrt_LIBRARY="$robotweax_dylib" \
    -DLibsrt_INCLUDE_DIR="$work/srt/include" \
    -DROBOTWEAX_OBS_MACOS_DEPS_PREFIX="$work/deps/obs" \
    -DCMAKE_PROJECT_obs-studio_INCLUDE="$repository/tests/obs/macos_qualification.cmake" \
    "${desktop_cmake[@]}" \
    -DENABLE_FRONTEND="$frontend" -DENABLE_BROWSER=OFF -DENABLE_SCRIPTING=OFF \
    -DENABLE_PLUGINS=ON -DENABLE_VLC=OFF -DENABLE_AJA=OFF \
    -DENABLE_NEW_MPEGTS_OUTPUT=ON \
    -DENABLE_SERVICE_UPDATES=OFF -DENABLE_WHATSNEW=OFF
"$repository/tools/python" "$repository/tests/obs/check_macos_cache.py" \
    "$work/obs-build/CMakeCache.txt" "$work/ffmpeg" "$work/srt" \
    ${cache_check[@]+"${cache_check[@]}"}
if [[ "$profile" == desktop ]]; then
    cmake --build "$work/obs-build" --config Release --parallel "$jobs" \
        --target obs-studio
else
    # Building ALL_BUILD also compiles OBS's unrelated Metal renderer, whose
    # pinned Swift source treats macOS 26 display-link deprecations as errors.
    cmake --build "$work/obs-build" --config Release --parallel "$jobs" \
        --target libobs libobs-opengl obs-ffmpeg obs-x264 obs-ffmpeg-mux
fi
"$repository/tools/python" "$repository/tests/obs/run_macos_smoke.py" \
    --obs-source "$obs_source" --obs-build "$work/obs-build" \
    --ffmpeg-prefix "$work/ffmpeg" --srt-prefix "$work/srt" \
    --reference-prefix "$work/deps/obs" --artifacts "$work/evidence" \
    | tee "$work/evidence/results.txt"
if [[ "$profile" == desktop ]]; then
    "$repository/tools/python" "$repository/tests/obs/run_macos_desktop_smoke.py" \
        --obs-build "$work/obs-build" --ffmpeg-prefix "$work/ffmpeg" \
        --srt-prefix "$work/srt" --reference-prefix "$work/deps/obs" \
        --fixture "$work/evidence/fixture.ts" \
        --reference-peer "$work/evidence/macos-reference-peer" \
        --artifacts "$work/evidence/desktop" \
        | tee "$work/evidence/desktop-results.txt"
fi
