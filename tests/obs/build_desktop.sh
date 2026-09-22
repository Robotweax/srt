#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 4 ]]; then
    echo "usage: $0 OBS_SOURCE SRT_PREFIX FFMPEG_PREFIX WORK_DIRECTORY" >&2
    exit 2
fi
repository="$(cd "$(dirname "$0")/../.." && pwd -P)"
source_directory="$(cd "$1" && pwd -P)"
srt_prefix="$(cd "$2" && pwd -P)"
ffmpeg_prefix="$(cd "$3" && pwd -P)"
[[ ! -e "$4" ]] || { echo "use a new desktop work directory" >&2; exit 1; }
mkdir -p "$4/evidence"
work="$(cd "$4" && pwd -P)"
exec > >(tee "$work/evidence/build.log") 2>&1
"$repository/tests/obs/configure.sh" "$source_directory" "$work/build" \
    "$srt_prefix" "$ffmpeg_prefix" "$work/obs" desktop
cmake --build "$work/build" --parallel "${JOBS:-2}"
cmake --install "$work/build"
test -x "$work/obs/bin/obs"
"$work/obs/bin/obs" --version
