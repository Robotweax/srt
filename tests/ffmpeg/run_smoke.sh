#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 FFMPEG_EXECUTABLE" >&2
    exit 2
fi

ffmpeg="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
if [[ ! -x "$ffmpeg" ]]; then
    echo "FFmpeg executable not found: $ffmpeg" >&2
    exit 2
fi

test_directory="$(mktemp -d "${TMPDIR:-/tmp}/robotweax-ffmpeg.XXXXXX")"
listener_pid=""
watchdog_pid=""
keep_artifacts="${ROBOTWEAX_FFMPEG_KEEP_ARTIFACTS:-0}"

cleanup() {
    local status=$?
    if [[ -n "$watchdog_pid" ]]; then
        kill "$watchdog_pid" 2>/dev/null || true
        wait "$watchdog_pid" 2>/dev/null || true
    fi
    if [[ -n "$listener_pid" ]]; then
        kill "$listener_pid" 2>/dev/null || true
        wait "$listener_pid" 2>/dev/null || true
    fi
    if [[ "$keep_artifacts" == "1" || $status -ne 0 ]]; then
        echo "FFmpeg smoke artifacts retained at $test_directory" >&2
    else
        rm -rf "$test_directory"
    fi
    return "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

"$ffmpeg" -version >"$test_directory/ffmpeg-version.log" 2>&1

fixture="$test_directory/fixture.ts"
expected_payload="$test_directory/expected.m2v"

"$ffmpeg" -nostdin -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=size=160x90:rate=25" \
    -t 1.2 -c:v mpeg2video -g 12 -f mpegts "$fixture"
"$ffmpeg" -nostdin -hide_banner -loglevel error -y \
    -i "$fixture" -map 0:v:0 -c copy -f data "$expected_payload"

allocate_port() {
    python3 -c \
        'import socket; value = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); value.bind(("127.0.0.1", 0)); print(value.getsockname()[1]); value.close()'
}

run_transfer() {
    local profile="$1"
    local security_query="$2"
    local port
    local received="$test_directory/received-$profile.ts"
    local actual_payload="$test_directory/actual-$profile.m2v"
    local listener_log="$test_directory/listener-$profile.log"
    local sender_log="$test_directory/sender-$profile.log"
    local common_query
    local listener_url
    local sender_url

    port="$(allocate_port)"
    common_query="transtype=live&messageapi=1&payload_size=1316&latency=80000&linger=2&timeout=5000000${security_query}"
    listener_url="srt://127.0.0.1:${port}?mode=listener&listen_timeout=5000000&${common_query}"
    sender_url="srt://127.0.0.1:${port}?mode=caller&connect_timeout=3000&${common_query}"

    "$ffmpeg" -nostdin -hide_banner -loglevel warning -y \
        -i "$listener_url" -map 0:v:0 -c copy -f mpegts "$received" \
        >"$test_directory/listener-$profile.out" 2>"$listener_log" &
    listener_pid=$!

    sleep 0.5
    if ! kill -0 "$listener_pid" 2>/dev/null; then
        wait "$listener_pid" || true
        listener_pid=""
        echo "FFmpeg listener exited before the $profile caller started" >&2
        cat "$listener_log" >&2
        return 1
    fi

    if ! "$ffmpeg" -nostdin -hide_banner -loglevel warning -re \
            -i "$fixture" -map 0:v:0 -c copy -f mpegts "$sender_url" \
            >"$test_directory/sender-$profile.out" 2>"$sender_log"; then
        echo "FFmpeg $profile caller failed" >&2
        cat "$sender_log" >&2
        cat "$listener_log" >&2
        return 1
    fi

    (
        sleep 15
        kill -TERM "$listener_pid" 2>/dev/null || true
    ) &
    watchdog_pid=$!
    set +e
    wait "$listener_pid"
    local listener_status=$?
    set -e
    listener_pid=""
    kill "$watchdog_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
    watchdog_pid=""

    if [[ $listener_status -ne 0 ]]; then
        echo "FFmpeg $profile listener failed with status $listener_status" >&2
        cat "$listener_log" >&2
        return 1
    fi

    "$ffmpeg" -nostdin -hide_banner -loglevel error -y \
        -i "$received" -map 0:v:0 -c copy -f data "$actual_payload"
    if ! cmp "$expected_payload" "$actual_payload"; then
        echo "FFmpeg $profile elementary stream differs after SRT transfer" >&2
        wc -c "$expected_payload" "$actual_payload" >&2
        cat "$sender_log" >&2
        cat "$listener_log" >&2
        return 1
    fi
}

run_transfer unencrypted ""
run_transfer aes-ctr \
    "&passphrase=robotweax-ffmpeg-test&pbkeylen=16&enforced_encryption=1"

echo "FFmpeg SRT smoke test passed for unencrypted and AES-CTR profiles"
