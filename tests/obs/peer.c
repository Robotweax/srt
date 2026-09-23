// SPDX-License-Identifier: MIT
#ifndef __APPLE__
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#ifdef __APPLE__
#include "obs.h"
#include "obs-service.h"
#include <mach-o/dyld.h>
#else
#include <obs/obs.h>
#include <obs/obs-service.h>
#endif
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static atomic_uint video_count, changed_count, audio_count, audible_count;
static uint64_t previous_hash;

static const char* test_name(void* unused)
{
    (void)unused;
    return "Robotweax qualification";
}

static void* filter_create(obs_data_t* settings, obs_source_t* source)
{
    (void)settings;
    return source;
}

static void filter_destroy(void* unused)
{
    (void)unused;
}

static struct obs_source_frame* inspect_video(
    void* unused, struct obs_source_frame* frame)
{
    (void)unused;
    if (!frame || !frame->data[0] || frame->width != 320
        || frame->height != 180)
        return frame;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned y = 0; y < frame->height; ++y)
        for (unsigned x = 0; x < frame->width; ++x)
            hash = (hash ^ frame->data[0][y * frame->linesize[0] + x])
                * UINT64_C(1099511628211);
    if (hash != previous_hash)
        atomic_fetch_add(&changed_count, 1);
    previous_hash = hash;
    atomic_fetch_add(&video_count, 1);
    return frame;
}

static void inspect_audio(void* unused, obs_source_t* source,
    const struct audio_data* audio, bool muted)
{
    (void)unused;
    (void)source;
    if (!audio->data[0] || muted)
        return;
    const float* samples = (const float*)audio->data[0];
    bool audible = false;
    for (unsigned i = 0; i < audio->frames; ++i)
        audible |= isfinite(samples[i]) && fabsf(samples[i]) > 0.001f;
    atomic_fetch_add(&audio_count, 1);
    if (audible)
        atomic_fetch_add(&audible_count, 1);
}

static void* service_create(obs_data_t* settings, obs_service_t* service)
{
    (void)service;
    obs_data_addref(settings);
    return settings;
}

static void service_destroy(void* data)
{
    obs_data_release(data);
}

static const char* service_protocol(void* unused)
{
    (void)unused;
    return "SRT";
}

static const char* service_info(void* data, uint32_t type)
{
    switch (type) {
    case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
        return obs_data_get_string(data, "url");
    case OBS_SERVICE_CONNECT_INFO_STREAM_ID:
        return obs_data_get_string(data, "streamid");
    case OBS_SERVICE_CONNECT_INFO_ENCRYPT_PASSPHRASE:
        return obs_data_get_string(data, "passphrase");
    default:
        return "";
    }
}

static void report(obs_output_t* output)
{
    printf("MEDIA video=%u changed=%u audio=%u audible=%u bytes=%llu\n",
        atomic_load(&video_count), atomic_load(&changed_count),
        atomic_load(&audio_count), atomic_load(&audible_count),
        (unsigned long long)(output ? obs_output_get_total_bytes(output) : 0));
    fflush(stdout);
}

static void maps(void)
{
#ifdef __APPLE__
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char* path = _dyld_get_image_name(i);
        if (path
            && (strstr(path, "robotweax-srt") || strstr(path, "libsrt")
                || strstr(path, "libavformat") || strstr(path, "obs-ffmpeg")))
            printf("MAP %s\n", path);
    }
#else
    FILE* file = fopen("/proc/self/maps", "r");
    if (!file)
        exit(2);
    char line[4096];
    while (fgets(line, sizeof(line), file))
        if (strstr(line, "/librobotweax-srt.so") || strstr(line, "/libsrt")
            || strstr(line, "/libavformat.so")
            || strstr(line, "/obs-ffmpeg.so"))
            printf("MAP %s", line);
    fclose(file);
#endif
    fflush(stdout);
}

static void binding(const char* path, const char* label)
{
    void* handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    void* symbol = handle ? dlsym(handle, "srt_startup") : NULL;
    Dl_info info;
    if (!symbol || !dladdr(symbol, &info)) {
        fprintf(stderr, "cannot resolve SRT provider through %s\n", path);
        exit(2);
    }
    printf("BINDING %s %s\n", label, info.dli_fname);
    dlclose(handle);
}

static void module(const char* prefix, const char* name)
{
    char path[4096], data[4096];
#ifdef __APPLE__
    (void)prefix;
    const char* bundle = strcmp(name, "obs-ffmpeg") == 0
        ? getenv("ROBOTWEAX_OBS_FFMPEG_PLUGIN")
        : getenv("ROBOTWEAX_OBS_X264_PLUGIN");
    if (!bundle)
        exit(2);
    snprintf(path, sizeof(path), "%s/Contents/MacOS/%s", bundle, name);
    snprintf(data, sizeof(data), "%s/Contents/Resources", bundle);
#else
    snprintf(path, sizeof(path), "%s/lib/obs-plugins/%s.so", prefix, name);
    snprintf(data, sizeof(data), "%s/share/obs/obs-plugins/%s", prefix, name);
#endif
    obs_module_t* loaded = NULL;
    if (obs_open_module(&loaded, path, data) != MODULE_SUCCESS
        || !obs_init_module(loaded)) {
        fprintf(stderr, "cannot load production module %s\n", path);
        exit(2);
    }
}

static bool stop_output(obs_output_t* output)
{
    // Failed starts have already signalled stop. Do not force-stop an
    // inactive output: release it and let the module join its start worker.
    if (!output || !obs_output_active(output))
        return true;
    obs_output_force_stop(output);
    for (int i = 0; i < 100 && obs_output_active(output); ++i) {
        struct timespec delay = {.tv_nsec = 50000000};
        nanosleep(&delay, NULL);
    }
    return !obs_output_active(output);
}

int main(int argc, char** argv)
{
    if (argc != 5) {
        fprintf(stderr,
            "usage: peer OBS_PREFIX INPUT OUTPUT_OR_DASH local|network\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!obs_startup("en-US", NULL, NULL))
        return 2;
    char graphics[4096];
#ifdef __APPLE__
    const char* graphics_module = getenv("ROBOTWEAX_OBS_GRAPHICS");
    if (!graphics_module)
        return 2;
    snprintf(graphics, sizeof(graphics), "%s", graphics_module);
#else
    snprintf(graphics, sizeof(graphics), "%s/lib/libobs-opengl.so", argv[1]);
#endif
    struct obs_video_info video = {.graphics_module = graphics,
        .fps_num = 25,
        .fps_den = 1,
        .base_width = 320,
        .base_height = 180,
        .output_width = 320,
        .output_height = 180,
        .output_format = VIDEO_FORMAT_I420,
        .colorspace = VIDEO_CS_709,
        .range = VIDEO_RANGE_PARTIAL,
        .scale_type = OBS_SCALE_BILINEAR};
    struct obs_audio_info audio = {
        .samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
    if (!obs_reset_audio(&audio)
        || obs_reset_video(&video) != OBS_VIDEO_SUCCESS)
        return 2;
    module(argv[1], "obs-ffmpeg");
    module(argv[1], "obs-x264");
    obs_post_load_modules();
    char plugin[4096];
#ifdef __APPLE__
    snprintf(plugin, sizeof(plugin), "%s/Contents/MacOS/obs-ffmpeg",
        getenv("ROBOTWEAX_OBS_FFMPEG_PLUGIN"));
#else
    snprintf(
        plugin, sizeof(plugin), "%s/lib/obs-plugins/obs-ffmpeg.so", argv[1]);
#endif
    binding(plugin, "native");
    const char* avformat = getenv("ROBOTWEAX_OBS_AVFORMAT");
    if (!avformat)
        return 2;
    binding(avformat, "ffmpeg");
    if (strcmp(argv[4], "network") == 0) {
        void (*set_av_log_level)(int) = dlsym(RTLD_DEFAULT, "av_log_set_level");
        if (set_av_log_level)
            set_av_log_level(48); // AV_LOG_DEBUG in the pinned FFmpeg API.
        else
            puts("SOURCE_DIAGNOSTICS unavailable");
    }
    struct obs_source_info filter_info = {.id = "robotweax-observer",
        .type = OBS_SOURCE_TYPE_FILTER,
        .output_flags = OBS_SOURCE_ASYNC_VIDEO,
        .get_name = test_name,
        .create = filter_create,
        .destroy = filter_destroy,
        .filter_video = inspect_video};
    obs_register_source(&filter_info);
    struct obs_service_info service_type = {.id = "robotweax-service",
        .get_name = test_name,
        .create = service_create,
        .destroy = service_destroy,
        .get_protocol = service_protocol,
        .get_connect_info = service_info};
    obs_register_service(&service_type);

    bool local = strcmp(argv[4], "local") == 0;
    obs_data_t* settings = obs_data_create();
    obs_data_set_bool(settings, "is_local_file", local);
    obs_data_set_string(settings, local ? "local_file" : "input", argv[2]);
    obs_data_set_string(settings, "input_format", "mpegts");
    if (!local)
        obs_data_set_string(settings, "ffmpeg_options",
            "probesize=131072 analyzeduration=3000000");
    obs_data_set_bool(settings, "looping", local);
    obs_data_set_bool(settings, "restart_on_activate", false);
    obs_data_set_int(settings, "reconnect_delay_sec", 1);
    obs_data_set_int(settings, "buffering_mb", 0);
    obs_source_t* source = obs_source_create(
        "ffmpeg_source", "qualification-media", settings, NULL);
    obs_data_release(settings);
    if (!source)
        return 2;
    obs_source_t* filter =
        obs_source_create_private("robotweax-observer", "observer", NULL);
    if (!filter)
        return 2;
    obs_source_filter_add(source, filter);
    obs_source_add_audio_capture_callback(source, inspect_audio, NULL);
    obs_set_output_source(0, source);

    obs_output_t* output = NULL;
    obs_service_t* service = NULL;
    obs_encoder_t *vencoder = NULL, *aencoder = NULL;
    if (strcmp(argv[3], "-") != 0) {
        settings = obs_data_create();
        obs_data_set_string(settings, "url", argv[3]);
        const char* streamid = getenv("ROBOTWEAX_OBS_STREAMID");
        const char* secret = getenv("ROBOTWEAX_OBS_PASSPHRASE");
        obs_data_set_string(
            settings, "streamid", streamid ? streamid : "robotweax-obs");
        obs_data_set_string(settings, "passphrase", secret ? secret : "");
        service = obs_service_create(
            "robotweax-service", "qualification-service", settings, NULL);
        obs_data_release(settings);
        settings = obs_data_create();
        obs_data_set_int(settings, "bitrate", 500);
        obs_data_set_int(settings, "keyint_sec", 1);
        obs_data_set_string(settings, "preset", "ultrafast");
        obs_data_set_string(settings, "tune", "zerolatency");
        vencoder = obs_video_encoder_create(
            "obs_x264", "qualification-video", settings, NULL);
        obs_data_release(settings);
        settings = obs_data_create();
        obs_data_set_int(settings, "bitrate", 128);
        aencoder = obs_audio_encoder_create(
            "ffmpeg_aac", "qualification-audio", settings, 0, NULL);
        obs_data_release(settings);
        output = obs_output_create(
            "ffmpeg_mpegts_muxer", "qualification-output", NULL, NULL);
        if (!service || !vencoder || !aencoder || !output)
            return 2;
        obs_encoder_set_video(vencoder, obs_get_video());
        obs_encoder_set_audio(aencoder, obs_get_audio());
        obs_output_set_video_encoder(output, vencoder);
        obs_output_set_audio_encoder(output, aencoder, 0);
        obs_output_set_service(output, service);
        if (local && !obs_output_start(output))
            return 2;
    }
    maps();
    puts("READY");
    if (!local)
        printf("SOURCE active=%d showing=%d media=%d\n",
            obs_source_active(source), obs_source_showing(source),
            obs_source_media_get_state(source));
    bool good = true;
    bool pending_output = output != NULL && !local;
    for (unsigned tick = 0; tick < 600; ++tick) {
        if (pending_output && atomic_load(&video_count) >= 20
            && atomic_load(&audible_count) >= 20) {
            if (!obs_output_start(output)) {
                good = false;
                break;
            }
            pending_output = false;
            puts("OUTPUT_STARTED");
        }
        fd_set inputs;
        FD_ZERO(&inputs);
        FD_SET(STDIN_FILENO, &inputs);
        struct timeval delay = {.tv_usec = 100000};
        int ready = select(STDIN_FILENO + 1, &inputs, NULL, NULL, &delay);
        if (ready > 0) {
            char command[80];
            if (!fgets(command, sizeof(command), stdin)
                || strncmp(command, "quit", 4) == 0)
                break;
            if (strncmp(command, "restart", 7) == 0 && output) {
                good = stop_output(output) && obs_output_start(output);
                if (!good)
                    break;
                puts("RESTARTED");
            }
        }
        if (tick % 10 == 0) {
            report(output);
            if (!local)
                printf("SOURCE active=%d showing=%d media=%d\n",
                    obs_source_active(source), obs_source_showing(source),
                    obs_source_media_get_state(source));
        }
    }
    report(output);
    maps();
    puts("TEARDOWN stop-output");
    good = stop_output(output) && good;
    puts("TEARDOWN release-output");
    obs_output_release(output);
    obs_encoder_release(vencoder);
    obs_encoder_release(aencoder);
    obs_service_release(service);
    obs_set_output_source(0, NULL);
    obs_source_remove_audio_capture_callback(source, inspect_audio, NULL);
    obs_source_filter_remove(source, filter);
    obs_source_release(filter);
    obs_source_release(source);
    puts("TEARDOWN obs-shutdown");
    obs_shutdown();
    puts("SHUTDOWN");
    return good ? 0 : 1;
}
