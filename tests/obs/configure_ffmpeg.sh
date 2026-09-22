#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 4 ]]; then
    echo "usage: $0 FFMPEG_SOURCE BUILD SRT_PREFIX INSTALL_PREFIX" >&2
    exit 2
fi
source_directory="$(cd "$1" && pwd -P)"
build="$2"
srt_prefix="$(cd "$3" && pwd -P)"
prefix="$4"
[[ "$(uname -s)" == Linux && "$build" == /* && "$prefix" == /* ]]
[[ ! -e "$build/ffbuild/config.mak" ]] || { echo "use a fresh FFmpeg build" >&2; exit 1; }
export PKG_CONFIG_PATH="$srt_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
[[ "$(pkg-config --variable=pcfiledir srt)" == "$srt_prefix/lib/pkgconfig" ]]
[[ "$(pkg-config --modversion srt)" == 1.5.7 ]]
[[ "$(pkg-config --libs-only-l srt)" == *-lrobotweax-srt* ]]
mkdir -p "$build"
cd "$build"
"$source_directory/configure" --prefix="$prefix" --libdir="$prefix/lib" \
    --disable-autodetect --disable-debug --disable-doc --disable-static \
    --enable-shared --disable-everything --disable-x86asm \
    --enable-gpl --enable-libx264 --enable-libsrt --enable-zlib \
    --enable-ffmpeg --enable-ffprobe --enable-avdevice --enable-avfilter \
    --enable-swscale --enable-swresample \
    --enable-encoder=libx264,aac,pcm_s16le,rawvideo,wrapped_avframe \
    --enable-decoder=h264,aac,pcm_s16le,rawvideo,wrapped_avframe,png \
    --enable-parser=h264,aac --enable-demuxer=mpegts,image2,image2pipe \
    --enable-muxer=mpegts,framehash,null,pcm_s16le \
    --enable-protocol=file,pipe,libsrt \
    --enable-filter=testsrc2,sine,aresample,scale,format,aformat,anull,null \
    --enable-indev=lavfi \
    --extra-ldflags="-Wl,-rpath,$srt_prefix/lib -Wl,-rpath,$prefix/lib"
