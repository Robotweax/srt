/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vlc/vlc.h>

typedef struct {
    unsigned char* planes[3];
    unsigned pitches[3];
    unsigned widths[3];
    unsigned heights[3];
    atomic_uint frames;
    atomic_int failed;
    atomic_int stopped;
    unsigned limit;
    FILE* frame_log;
} capture_t;

static double now(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec + value.tv_nsec / 1e9;
}

static unsigned format(void** opaque, char* chroma, unsigned* width,
    unsigned* height, unsigned* pitches, unsigned* lines)
{
    capture_t* capture = *opaque;
    if (*width != 128 || *height != 96) {
        atomic_store(&capture->failed, 1);
        return 0;
    }
    memcpy(chroma, "I420", 4);
    for (unsigned plane = 0; plane < 3; ++plane) {
        capture->widths[plane] = plane == 0 ? *width : *width / 2;
        capture->heights[plane] = plane == 0 ? *height : *height / 2;
        pitches[plane] = (capture->widths[plane] + 31) & ~31u;
        lines[plane] = (capture->heights[plane] + 31) & ~31u;
        capture->pitches[plane] = pitches[plane];
        if (posix_memalign((void**)&capture->planes[plane], 32,
                pitches[plane] * lines[plane])
            != 0) {
            for (unsigned allocated = 0; allocated < plane; ++allocated) {
                free(capture->planes[allocated]);
                capture->planes[allocated] = NULL;
            }
            atomic_store(&capture->failed, 1);
            return 0;
        }
    }
    return 1;
}

static void cleanup_video(void* opaque)
{
    capture_t* capture = opaque;
    for (unsigned plane = 0; plane < 3; ++plane) {
        free(capture->planes[plane]);
        capture->planes[plane] = NULL;
    }
}

static void* lock_video(void* opaque, void** planes)
{
    capture_t* capture = opaque;
    for (unsigned plane = 0; plane < 3; ++plane) {
        planes[plane] = capture->planes[plane];
    }
    return capture;
}

static void unlock_video(void* opaque, void* picture, void* const* planes)
{
    (void)picture;
    capture_t* capture = opaque;
    if (atomic_load(&capture->stopped)
        || (capture->limit != 0
            && atomic_load(&capture->frames) >= capture->limit)) {
        return;
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned plane = 0; plane < 3; ++plane) {
        const unsigned char* pixels = planes[plane];
        for (unsigned y = 0; y < capture->heights[plane]; ++y) {
            for (unsigned x = 0; x < capture->widths[plane]; ++x) {
                hash ^= pixels[y * capture->pitches[plane] + x];
                hash *= UINT64_C(1099511628211);
            }
        }
    }
    fprintf(capture->frame_log, "FRAME %016llx\n", (unsigned long long)hash);
    fflush(capture->frame_log);
    atomic_fetch_add(&capture->frames, 1);
}

/* Resolve through the actual VLC module, not a direct SRT link. Keep the
 * module open while libVLC runs, then check the process mappings as well. */
static void* verify_provider(void)
{
    const char* plugin = getenv("ROBOTWEAX_VLC_PLUGIN");
    const char* expected = getenv("ROBOTWEAX_SRT_LIBRARY_DIR");
    if (plugin == NULL || expected == NULL) {
        return NULL;
    }
    void* module = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
    Dl_info info;
    void* symbol = module == NULL ? NULL : dlsym(module, "srt_startup");
    if (symbol == NULL || dladdr(symbol, &info) == 0) {
        fprintf(stderr, "cannot resolve SRT from VLC module: %s\n", dlerror());
        if (module != NULL) {
            dlclose(module);
        }
        return NULL;
    }
    char* actual = realpath(info.dli_fname, NULL);
    char* slash = actual == NULL ? NULL : strrchr(actual, '/');
    printf("PROVIDER plugin=%s library=%s\n", plugin,
        actual == NULL ? "unresolved" : actual);
    if (slash != NULL) {
        *slash = '\0';
    }
    int valid = slash != NULL && strcmp(actual, expected) == 0;
    free(actual);
    if (!valid) {
        dlclose(module);
        return NULL;
    }
    return module;
}

static int verify_loaded_libraries(void)
{
    const char* expected = getenv("ROBOTWEAX_SRT_LIBRARY_DIR");
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps == NULL || expected == NULL) {
        if (maps != NULL) {
            fclose(maps);
        }
        return 0;
    }
    char* line = NULL;
    size_t capacity = 0;
    int found = 0;
    int valid = 1;
    while (getline(&line, &capacity, maps) >= 0) {
        char* path = strchr(line, '/');
        if (path == NULL
            || (strstr(path, "/librobotweax-srt.so") == NULL
                && strstr(path, "/libsrt.so") == NULL)) {
            continue;
        }
        path[strcspn(path, "\n")] = '\0';
        char* actual = realpath(path, NULL);
        char* slash = actual == NULL ? NULL : strrchr(actual, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
        valid &= slash != NULL && strcmp(actual, expected) == 0;
        found = 1;
        free(actual);
    }
    free(line);
    fclose(maps);
    if (!valid || !found) {
        fprintf(
            stderr, "unexpected or missing SRT provider in process mappings\n");
        return 0;
    }
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        fprintf(stderr,
            "usage: peer decode URI FRAME_LIMIT | observe URI 0 | send FILE "
            "DEST | stop URI "
            "CYCLES\n");
        return 2;
    }
    int sending = strcmp(argv[1], "send") == 0;
    int stopping = strcmp(argv[1], "stop") == 0;
    int observing = strcmp(argv[1], "observe") == 0;
    if (!sending && !stopping && !observing && strcmp(argv[1], "decode") != 0) {
        return 2;
    }
    void* module = verify_provider();
    if (module == NULL) {
        return 1;
    }
    const char* passphrase = getenv("ROBOTWEAX_VLC_TEST_PASSPHRASE");
    char secret_option[256];
    snprintf(secret_option, sizeof(secret_option), "--passphrase=%s",
        passphrase != NULL ? passphrase : "");
    const char* streamid = getenv("ROBOTWEAX_VLC_TEST_STREAMID");
    char stream_option[256];
    snprintf(stream_option, sizeof(stream_option), "--streamid=%s",
        streamid != NULL ? streamid : "robotweax-test");
    const char* options[] = {"--no-plugins-cache", "--ignore-config",
        "--no-audio", "--no-video-title-show", "--no-osd",
        "--no-drop-late-frames", "--no-skip-frames", "--network-caching=100",
        "--file-caching=100", "--verbose=2", stream_option, "--latency=80",
        "--key-length=16", secret_option};
    libvlc_instance_t* instance =
        libvlc_new((int)(sizeof(options) / sizeof(options[0])), options);
    if (instance == NULL) {
        dlclose(module);
        return 1;
    }
    const char* frame_path = getenv("ROBOTWEAX_VLC_FRAME_LOG");
    FILE* frame_log = frame_path == NULL ? NULL : fopen(frame_path, "w");
    if (frame_log == NULL) {
        libvlc_release(instance);
        dlclose(module);
        return 1;
    }
    unsigned cycles = stopping ? (unsigned)strtoul(argv[3], NULL, 10) : 1;
    unsigned target =
        !sending && !stopping ? (unsigned)strtoul(argv[3], NULL, 10) : 0;
    int result = 0;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        capture_t capture = {0};
        capture.limit = target;
        capture.frame_log = frame_log;
        libvlc_media_t* media = strstr(argv[2], "://") != NULL
            ? libvlc_media_new_location(instance, argv[2])
            : libvlc_media_new_path(instance, argv[2]);
        if (media == NULL) {
            result = 1;
            break;
        }
        libvlc_media_add_option(media, ":demux=ts");
        if (sending) {
            char output[1024];
            snprintf(output, sizeof(output),
                ":sout=#standard{access=srt,mux=ts,dst=%s}", argv[3]);
            libvlc_media_add_option(media, output);
            libvlc_media_add_option(media, ":input-repeat=1000");
            libvlc_media_add_option(media, ":sout-keep");
        }
        libvlc_media_player_t* player =
            libvlc_media_player_new_from_media(media);
        if (player == NULL) {
            libvlc_media_release(media);
            result = 1;
            break;
        }
        if (!sending) {
            libvlc_video_set_callbacks(
                player, lock_video, unlock_video, NULL, &capture);
            libvlc_video_set_format_callbacks(player, format, cleanup_video);
        }
        result = libvlc_media_player_play(player) == 0 ? 0 : 1;
        printf("STARTED cycle=%u\n", cycle);
        fflush(stdout);
        if (sending || stopping || observing) {
            if (getchar() != '\n') {
                result = 1;
            }
        } else {
            double deadline = now() + 20;
            while (result == 0 && now() < deadline) {
                libvlc_state_t state = libvlc_media_player_get_state(player);
                if (state == libvlc_Error || atomic_load(&capture.failed)) {
                    result = 1;
                    break;
                }
                if ((target != 0 && atomic_load(&capture.frames) >= target)
                    || state == libvlc_Ended) {
                    break;
                }
                usleep(10000);
            }
            if (atomic_load(&capture.frames) == 0
                || (target != 0 && atomic_load(&capture.frames) < target)) {
                result = 1;
            }
        }
        libvlc_media_stats_t stats = {0};
        libvlc_media_get_stats(media, &stats);
        if (!verify_loaded_libraries()) {
            result = 1;
        }
        atomic_store(&capture.stopped, 1);
        double started = now();
        libvlc_media_player_stop(player);
        double elapsed = now() - started;
        printf("STOP cycle=%u seconds=%.3f frames=%u read=%d sent=%d\n", cycle,
            elapsed, atomic_load(&capture.frames), stats.i_read_bytes,
            stats.i_sent_bytes);
        fflush(stdout);
        if (elapsed > 2.0) {
            result = 1;
        }
        libvlc_media_player_release(player);
        libvlc_media_release(media);
        if (result != 0) {
            break;
        }
    }
    libvlc_release(instance);
    if (fclose(frame_log) != 0) {
        result = 1;
    }
    dlclose(module);
    return result;
}
