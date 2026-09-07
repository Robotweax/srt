#!/bin/sh
# Trace only this receiver and its threads. Never decode packet buffers.
set -eu
: "${SRT_TIMING_PEER:?missing receiver executable}"
: "${SRT_TIMING_TRACE_FILE:?missing trace output path}"
exec strace -f -ttt -T \
    -e trace=futex,sendto,recvfrom,sendmsg,recvmsg,poll,ppoll,pselect6,epoll_wait,clock_nanosleep,nanosleep \
    -e raw=sendto,recvfrom,sendmsg,recvmsg \
    -o "$SRT_TIMING_TRACE_FILE" "$SRT_TIMING_PEER" "$@"
