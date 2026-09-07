#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 FFMPEG_SOURCE ROBOTWEAX_INSTALL_PREFIX" >&2
    exit 2
fi

ffmpeg_source="$(cd "$1" && pwd)"
robotweax_prefix="$(cd "$2" && pwd)"
pkgconfig_directory="$robotweax_prefix/lib/pkgconfig"

if [[ ! -x "$ffmpeg_source/configure" ]]; then
    echo "FFmpeg configure script not found: $ffmpeg_source/configure" >&2
    exit 2
fi
if [[ ! -f "$pkgconfig_directory/srt.pc" ]]; then
    echo "Robotweax srt.pc not found: $pkgconfig_directory/srt.pc" >&2
    exit 2
fi

export PKG_CONFIG_PATH="$pkgconfig_directory${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

compatible_version="$(pkg-config --modversion srt)"
metadata_directory="$(pkg-config --variable=pcfiledir srt)"
if [[ "$compatible_version" != "1.5.7" ]]; then
    echo "expected SRT compatibility version 1.5.7, got $compatible_version" >&2
    exit 1
fi
if [[ "$metadata_directory" != "$pkgconfig_directory" ]]; then
    echo "pkg-config selected an unexpected srt.pc: $metadata_directory" >&2
    exit 1
fi

case "$(uname -s)" in
    Darwin)
        runtime_linker_flags="-Wl,-rpath,$robotweax_prefix/lib"
        ;;
    Linux)
        runtime_linker_flags="-Wl,-rpath,$robotweax_prefix/lib"
        ;;
    *)
        echo "the FFmpeg MVP configure helper supports Linux and macOS" >&2
        exit 2
        ;;
esac

cd "$ffmpeg_source"
./configure \
    --disable-autodetect \
    --disable-debug \
    --disable-doc \
    --disable-everything \
    --disable-x86asm \
    --enable-decoder=wrapped_avframe \
    --enable-demuxer=mpegts \
    --enable-encoder=mpeg2video \
    --enable-ffmpeg \
    --enable-filter=testsrc2 \
    --enable-indev=lavfi \
    --enable-libsrt \
    --enable-muxer=data \
    --enable-muxer=mpegts \
    --enable-parser=mpegvideo \
    --enable-protocol=file \
    --enable-protocol=pipe \
    --enable-protocol=libsrt \
    --extra-ldflags="$runtime_linker_flags"
