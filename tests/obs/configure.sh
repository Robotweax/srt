#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 5 || $# -gt 6 ]]; then
    echo "usage: $0 OBS_SOURCE BUILD SRT_PREFIX FFMPEG_PREFIX INSTALL_PREFIX [headless|desktop]" >&2
    exit 2
fi
repository="$(cd "$(dirname "$0")/../.." && pwd -P)"
source_directory="$(cd "$1" && pwd -P)"
build="$2"
srt_prefix="$(cd "$3" && pwd -P)"
ffmpeg_prefix="$(cd "$4" && pwd -P)"
prefix="$5"
profile="${6:-headless}"
frontend=OFF
desktop_options=()
case "$profile" in
    headless) ;;
    desktop)
        frontend=ON
        desktop_options=(-DENABLE_SERVICE_UPDATES=OFF -DENABLE_WHATSNEW=OFF)
        ;;
    *) echo "unknown OBS profile: $profile" >&2; exit 2 ;;
esac
[[ "$(uname -s)" == Linux && "$build" == /* && "$prefix" == /* ]]
[[ ! -e "$build/CMakeCache.txt" ]] || { echo "use a fresh OBS build" >&2; exit 1; }
[[ "$(git -C "$source_directory" rev-parse HEAD)" == ba2f32bdf791005443988a4955e963663e16b1ed ]]
[[ -z "$(git -C "$source_directory" status --porcelain --untracked-files=no -- . ':!plugins/CMakeLists.txt')" ]]
"$repository/tools/python" "$repository/tests/obs/prepare_source.py" "$source_directory" --profile "$profile"
export PKG_CONFIG_PATH="$srt_prefix/lib/pkgconfig:$ffmpeg_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
[[ "$(pkg-config --variable=pcfiledir srt)" == "$srt_prefix/lib/pkgconfig" ]]
[[ "$(pkg-config --modversion srt)" == 1.5.7 ]]
[[ "$(pkg-config --libs-only-l srt)" == *-lrobotweax-srt* ]]
for component in libavcodec libavdevice libavfilter libavformat libavutil libswscale libswresample; do
    [[ "$(pkg-config --variable=pcfiledir "$component")" == "$ffmpeg_prefix/lib/pkgconfig" ]]
done
# A SHA-only checkout has no release tags for OBS's git-describe version probe.
# The validated source revision above fixes the version; label this build profile.
cmake -S "$source_directory" -B "$build" -G Ninja \
    -DOBS_VERSION_OVERRIDE=32.2.2-robotweax-qualification \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH="$ffmpeg_prefix;$srt_prefix" \
    -DCMAKE_INSTALL_RPATH="$prefix/lib;$ffmpeg_prefix/lib;$srt_prefix/lib" \
    -DLibsrt_LIBRARY="$srt_prefix/lib/librobotweax-srt.so" \
    -DLibsrt_INCLUDE_DIR="$(pkg-config --variable=includedir srt)" \
    -DCMAKE_PROJECT_obs-studio_INCLUDE="$repository/tests/obs/qualification.cmake" \
    -DENABLE_FRONTEND="$frontend" -DENABLE_SCRIPTING=OFF -DENABLE_PLUGINS=ON \
    -DENABLE_NEW_MPEGTS_OUTPUT=ON \
    -DENABLE_WAYLAND=OFF -DENABLE_PULSEAUDIO=OFF "${desktop_options[@]}"
