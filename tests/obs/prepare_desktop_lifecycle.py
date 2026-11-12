#!/usr/bin/env python3
"""Apply a version-locked, independently authored OBS desktop lifecycle fix.

Release the previous FFmpeg state before reinitializing it and after a failed
start, and flush the custom I/O context before the URL context it writes
through is closed and freed.
"""

import argparse
import hashlib
from pathlib import Path

TARGET = "plugins/obs-ffmpeg/obs-ffmpeg-mpegts.c"
ORIGINAL_SHA256 = "88283cf331298ded95f1d80c494058d8f728770aeeaeb6d29400bc2d0d617536"
INIT = b"bool ffmpeg_mpegts_data_init("
CONFIG = b"static bool set_config("
RESET = b"\tmemset(data, 0, sizeof(struct ffmpeg_data));"
RELEASE = b"\tffmpeg_mpegts_data_free(stream, data);"
FAIL = b"fail:\n"
CLEAN_FAILURE = b"fail:\n\tffmpeg_mpegts_data_free(stream, &stream->ff_data);\n"
CLOSE = b"static void close_mpegts_url("
CLOSE_URL = (
    b"\t/* close rist or srt URLs ; free URLContext */\n"
    b"\tif (is_rist) {\n"
    b"\t\terr = librist_close(h);\n"
    b"\t} else {\n"
    b"\t\terr = libsrt_close(h);\n"
    b"\t}\n"
    b"\tav_freep(&h->priv_data);\n"
    b"\tav_freep(&h);\n"
    b"\th = NULL;\n"
    b"\n"
)
CLOSE_IO = (
    b"\t/* close custom avio_context for srt or rist */\n"
    b"\tAVIOContext *s = stream->s;\n"
    b"\tif (!s)\n"
    b"\t\treturn;\n"
    b"\n"
    b"\tavio_flush(s);\n"
    b"\ts->opaque = NULL;\n"
    b"\tav_freep(&s->buffer);\n"
    b"\tavio_context_free(&s);\n"
    b"\ts = NULL;\n"
    b"\n"
)
# The flush writes through the URLContext, so it must precede its release.
FLUSH_FIRST = (
    b"\t/* flush the custom avio_context while its URLContext is alive */\n"
    b"\tAVIOContext *s = stream->s;\n"
    b"\tif (s) {\n"
    b"\t\tavio_flush(s);\n"
    b"\t\ts->opaque = NULL;\n"
    b"\t\tav_freep(&s->buffer);\n"
    b"\t\tavio_context_free(&s);\n"
    b"\t}\n"
    b"\n"
) + CLOSE_URL


def transform(original):
    """Existing start serialization joins the prior writer before this reset."""
    before, separator, after = original.partition(INIT)
    if not separator or after.count(RESET) != 1:
        raise RuntimeError("unrecognized OBS MPEG-TS initialization")
    fixed = before + separator + after.replace(RESET, RELEASE, 1)
    before, separator, after = fixed.partition(CONFIG)
    if not separator or FAIL not in after:
        raise RuntimeError("unrecognized OBS MPEG-TS configuration")
    fixed = before + separator + after.replace(FAIL, CLEAN_FAILURE, 1)
    before, separator, after = fixed.partition(CLOSE)
    if not separator or after.count(CLOSE_URL + CLOSE_IO) != 1:
        raise RuntimeError("unrecognized OBS MPEG-TS URL cleanup")
    return before + separator + after.replace(CLOSE_URL + CLOSE_IO, FLUSH_FIRST, 1)


def prepare(source):
    target = source / TARGET
    current = target.read_bytes()
    normalized = current.replace(b"\r\n", b"\n")
    # Undo only our exact additions for verification; unknown edits are rejected.
    before, separator, after = normalized.partition(INIT)
    restored = before + separator + after.replace(RELEASE, RESET, 1)
    restored = restored.replace(CLEAN_FAILURE, FAIL, 1)
    restored = restored.replace(FLUSH_FIRST, CLOSE_URL + CLOSE_IO, 1)
    if hashlib.sha256(restored).hexdigest() != ORIGINAL_SHA256:
        raise RuntimeError("unrecognized OBS MPEG-TS source; use the pinned checkout")
    fixed = transform(restored)
    if normalized == fixed:
        return False
    if normalized != restored:
        raise RuntimeError("partially applied OBS MPEG-TS lifecycle correction")
    target.write_bytes(fixed)
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    prepare(parser.parse_args().source)
