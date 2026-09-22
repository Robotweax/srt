#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "usage: $0 GST_SOURCE GST_BUILD SRT_PREFIX GST_INSTALL_PREFIX" >&2
    exit 2
fi

gst_source="$(cd "$1" && pwd -P)"
gst_build="$2"
srt_prefix="$(cd "$3" && pwd -P)"
gst_prefix="$4"
meson="${MESON:-meson}"
pc_directory="$srt_prefix/lib/pkgconfig"

# The same pinned upstream source can also qualify the reference provider.
# Never fall back to another srt.pc or to an unversioned system library.
[[ -f "$pc_directory/srt.pc" ]] || {
    echo "missing $pc_directory/srt.pc (install with libdir=lib)" >&2
    exit 1
}
export PKG_CONFIG_PATH="$pc_directory${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
[[ "$(pkg-config --variable=pcfiledir srt)" == "$pc_directory" ]] || exit 1
pkg-config --atleast-version=1.3.0 srt
[[ "$gst_prefix" == /* && "$gst_build" == /* ]] || {
    echo "GST_BUILD and GST_INSTALL_PREFIX must be absolute paths" >&2
    exit 2
}
if [[ -e "$gst_build/meson-private/coredata.dat" ]]; then
    echo "use a fresh build directory so cached dependencies cannot select another SRT provider" >&2
    exit 1
fi
runtime_paths="-Wl,-rpath,$srt_prefix/lib,-rpath,$gst_prefix/lib"
link_options=("-Dc_link_args=$runtime_paths")
case "$(uname -s)" in
    Darwin) link_options+=("-Dobjc_link_args=$runtime_paths") ;;
    Linux) ;;
    *) echo "this qualification helper supports Linux and macOS" >&2; exit 2 ;;
esac

"$meson" setup "$gst_build" "$gst_source" \
    --prefix="$gst_prefix" --libdir=lib --buildtype=release \
    --wrap-mode=nofallback -Dauto_features=disabled \
    -Dbase=enabled -Dbad=enabled -Dgood=disabled -Dugly=disabled \
    -Dtools=enabled -Dbuild-tools-source=system -Dgstreamer:check=enabled \
    -Dgst-plugins-base:app=enabled -Dgst-plugins-bad:srt=enabled \
    "${link_options[@]}"
