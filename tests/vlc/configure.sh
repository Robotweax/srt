#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "usage: $0 VLC_SOURCE VLC_BUILD SRT_PREFIX VLC_INSTALL_PREFIX" >&2
    exit 2
fi
source_directory="$(cd "$1" && pwd -P)"
build="$2"
srt_prefix="$(cd "$3" && pwd -P)"
prefix="$4"
[[ "$(uname -s)" == Linux ]] || { echo "this profile supports Linux" >&2; exit 2; }
[[ "$build" == /* && "$prefix" == /* ]] || {
    echo "build and install paths must be absolute" >&2; exit 2;
}
[[ ! -e "$build/config.status" ]] || {
    echo "use a fresh VLC build directory" >&2; exit 1;
}
pc_directory="$srt_prefix/lib/pkgconfig"
[[ -f "$pc_directory/srt.pc" ]] || { echo "missing $pc_directory/srt.pc" >&2; exit 1; }
export PKG_CONFIG_PATH="$pc_directory${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
[[ "$(pkg-config --variable=pcfiledir srt)" == "$pc_directory" ]]
pkg-config --atleast-version=1.3.0 srt
if [[ ! -x "$source_directory/configure" ]]; then
    (cd "$source_directory" && ./bootstrap)
fi
mkdir -p "$build"
cd "$build"
export LDFLAGS="-Wl,-rpath,$srt_prefix/lib -Wl,-rpath,$prefix/lib ${LDFLAGS:-}"
"$source_directory/configure" --prefix="$prefix" --libdir="$prefix/lib" \
    --with-contrib=no --enable-srt --enable-libmpeg2 --enable-dvbpsi \
    --disable-avcodec --disable-avformat --disable-swscale --disable-postproc \
    --disable-lua --disable-qt --disable-skins2 --disable-nls \
    --disable-xcb --disable-wayland --disable-gles2 \
    --disable-dbus --disable-pulse --disable-alsa --disable-jack \
    --disable-vdpau --disable-libva --disable-chromecast \
    --disable-update-check --disable-notify
