// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>

#include <obs.h>
#include <util/platform.h>

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG video_count;
static volatile LONG changed_count;
static volatile LONG audio_count;
static volatile LONG audible_count;
static uint64_t previous_hash;
static FILE* evidence;

static void stage(const char* name)
{
    fprintf(stderr, "STAGE %s\n", name);
    fflush(stderr);
}

static const char* test_name(void* unused)
{
    (void)unused;
    return "Robotweax Windows qualification";
}

static void* source_create(obs_data_t* settings, obs_source_t* source)
{
    (void)settings;
    return source;
}

static void source_destroy(void* unused)
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
        InterlockedIncrement(&changed_count);
    previous_hash = hash;
    InterlockedIncrement(&video_count);
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
    InterlockedIncrement(&audio_count);
    if (audible)
        InterlockedIncrement(&audible_count);
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

static void path(
    char* output, size_t size, const char* prefix, const char* tail)
{
    int written = snprintf(output, size, "%s/%s", prefix, tail);
    if (written < 0 || (size_t)written >= size) {
        fprintf(stderr, "path is too long\n");
        exit(2);
    }
}

static void module(const char* prefix, const char* name)
{
    char binary[MAX_PATH], data[MAX_PATH], tail[MAX_PATH];
    int written =
        snprintf(tail, sizeof(tail), "obs-plugins/64bit/%s.dll", name);
    if (written < 0 || (size_t)written >= sizeof(tail))
        exit(2);
    path(binary, sizeof(binary), prefix, tail);
    written = snprintf(tail, sizeof(tail), "data/obs-plugins/%s", name);
    if (written < 0 || (size_t)written >= sizeof(tail))
        exit(2);
    path(data, sizeof(data), prefix, tail);
    obs_module_t* loaded = NULL;
    if (obs_open_module(&loaded, binary, data) != MODULE_SUCCESS
        || !obs_init_module(loaded)) {
        fprintf(stderr, "cannot load production module %s\n", binary);
        exit(2);
    }
}

static bool interesting(const char* value)
{
    char lower[MAX_PATH];
    size_t length = strlen(value);
    if (length >= sizeof(lower))
        return false;
    for (size_t i = 0; i <= length; ++i)
        lower[i] = (char)tolower((unsigned char)value[i]);
    return strstr(lower, "\\srt.dll") || strstr(lower, "/srt.dll")
        || strstr(lower, "avformat-") || strstr(lower, "obs-ffmpeg.dll");
}

static void modules(void)
{
    HMODULE handles[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(
            GetCurrentProcess(), handles, sizeof(handles), &needed)) {
        fprintf(stderr, "cannot enumerate process modules\n");
        exit(2);
    }
    unsigned count = needed / sizeof(handles[0]);
    if (count > sizeof(handles) / sizeof(handles[0]))
        count = sizeof(handles) / sizeof(handles[0]);
    for (unsigned i = 0; i < count; ++i) {
        char value[MAX_PATH];
        if (GetModuleFileNameExA(
                GetCurrentProcess(), handles[i], value, sizeof(value))
            && interesting(value))
            fprintf(evidence, "MODULE %s\n", value);
    }
    HMODULE provider = GetModuleHandleA("srt.dll");
    char value[MAX_PATH];
    FARPROC startup = provider ? GetProcAddress(provider, "srt_startup") : NULL;
    if (!provider || !startup
        || !GetModuleFileNameA(provider, value, sizeof(value))) {
        fprintf(stderr, "cannot resolve loaded SRT provider\n");
        exit(2);
    }
    fprintf(evidence, "BINDING %s\n", value);
    fflush(evidence);
}

static void report(obs_output_t* output)
{
    fprintf(evidence,
        "MEDIA video=%ld changed=%ld audio=%ld audible=%ld bytes=%llu\n",
        InterlockedAdd(&video_count, 0), InterlockedAdd(&changed_count, 0),
        InterlockedAdd(&audio_count, 0), InterlockedAdd(&audible_count, 0),
        (unsigned long long)(output ? obs_output_get_total_bytes(output) : 0));
    fflush(evidence);
}

static void emit_synthetic(
    obs_source_t* source, uint64_t timestamp, unsigned index)
{
    struct obs_source_frame* frame =
        obs_source_frame_create(VIDEO_FORMAT_I420, 320, 180);
    if (!frame)
        exit(2);
    frame->full_range = false;
    if (!video_format_get_parameters_for_format(VIDEO_CS_709,
            VIDEO_RANGE_PARTIAL, VIDEO_FORMAT_I420, frame->color_matrix,
            frame->color_range_min, frame->color_range_max))
        exit(2);
    for (unsigned y = 0; y < 180; ++y)
        for (unsigned x = 0; x < 320; ++x)
            frame->data[0][y * frame->linesize[0] + x] =
                (uint8_t)((x + y + index * 5) & 255U);
    for (unsigned y = 0; y < 90; ++y)
        for (unsigned x = 0; x < 160; ++x) {
            frame->data[1][y * frame->linesize[1] + x] =
                (uint8_t)(64U + ((index + x) & 63U));
            frame->data[2][y * frame->linesize[2] + x] =
                (uint8_t)(128U + ((index + y) & 63U));
        }
    frame->timestamp = timestamp;
    obs_source_output_video(source, frame);
    obs_source_frame_destroy(frame);

    float left[1920], right[1920];
    for (unsigned sample = 0; sample < 1920; ++sample) {
        double position = (double)(index * 1920U + sample) / 48000.0;
        left[sample] =
            (float)(sin(position * 2.0 * 3.141592653589793 * 997.0) * 0.2);
        right[sample] = left[sample];
    }
    struct obs_source_audio audio = {0};
    audio.data[0] = (const uint8_t*)left;
    audio.data[1] = (const uint8_t*)right;
    audio.frames = 1920;
    audio.speakers = SPEAKERS_STEREO;
    audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    audio.samples_per_sec = 48000;
    audio.timestamp = timestamp;
    obs_source_output_audio(source, &audio);
}

static bool command_ready(void)
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD available = 0;
    return input != INVALID_HANDLE_VALUE
        && PeekNamedPipe(input, NULL, 0, NULL, &available, NULL) && available;
}

static bool stop_output(obs_output_t* output)
{
    if (!output || !obs_output_active(output))
        return true;
    obs_output_force_stop(output);
    for (unsigned attempt = 0; attempt < 100 && obs_output_active(output);
        ++attempt)
        Sleep(50);
    return !obs_output_active(output);
}

int main(int argc, char** argv)
{
    stage("main");
    if (argc != 5
        || (strcmp(argv[2], "send") != 0 && strcmp(argv[2], "receive") != 0)) {
        fprintf(stderr,
            "usage: windows-obs-peer OBS_PREFIX send|receive URL "
            "EXPECTED_PROVIDER\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* evidence_path = getenv("ROBOTWEAX_OBS_EVIDENCE");
    if (!evidence_path || !(evidence = fopen(evidence_path, "w"))) {
        fprintf(stderr, "cannot open OBS evidence file\n");
        return 2;
    }
    stage("obs-startup");
    if (!obs_startup("en-US", NULL, NULL))
        return 2;
    stage("obs-started");
    char graphics[MAX_PATH];
    path(graphics, sizeof(graphics), argv[1], "bin/64bit/libobs-d3d11.dll");
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
    stage("audio-video-ready");
    module(argv[1], "obs-ffmpeg");
    stage("ffmpeg-module-ready");
    module(argv[1], "obs-x264");
    stage("x264-module-ready");
    obs_post_load_modules();
    stage("modules-ready");

    struct obs_source_info synthetic_info = {.id = "robotweax-synthetic",
        .type = OBS_SOURCE_TYPE_INPUT,
        .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
        .get_name = test_name,
        .create = source_create,
        .destroy = source_destroy};
    struct obs_source_info filter_info = {.id = "robotweax-observer",
        .type = OBS_SOURCE_TYPE_FILTER,
        .output_flags = OBS_SOURCE_ASYNC_VIDEO,
        .get_name = test_name,
        .create = source_create,
        .destroy = source_destroy,
        .filter_video = inspect_video};
    struct obs_service_info service_type = {.id = "robotweax-service",
        .get_name = test_name,
        .create = service_create,
        .destroy = service_destroy,
        .get_protocol = service_protocol,
        .get_connect_info = service_info};
    obs_register_source(&synthetic_info);
    obs_register_source(&filter_info);
    obs_register_service(&service_type);
    stage("types-registered");

    bool sending = strcmp(argv[2], "send") == 0;
    obs_source_t* source = NULL;
    obs_source_t* filter = NULL;
    obs_output_t* output = NULL;
    obs_service_t* service = NULL;
    obs_encoder_t *vencoder = NULL, *aencoder = NULL;
    obs_data_t* settings = obs_data_create();
    if (sending) {
        source = obs_source_create_private(
            "robotweax-synthetic", "synthetic", settings);
    } else {
        obs_data_set_bool(settings, "is_local_file", false);
        obs_data_set_string(settings, "input", argv[3]);
        obs_data_set_string(settings, "input_format", "mpegts");
        obs_data_set_int(settings, "reconnect_delay_sec", 1);
        obs_data_set_int(settings, "buffering_mb", 0);
        source = obs_source_create(
            "ffmpeg_source", "qualification-media", settings, NULL);
    }
    obs_data_release(settings);
    if (!source)
        return 2;
    stage("source-ready");
    filter = obs_source_create_private("robotweax-observer", "observer", NULL);
    if (!filter)
        return 2;
    obs_source_filter_add(source, filter);
    if (!sending)
        obs_source_add_audio_capture_callback(source, inspect_audio, NULL);
    obs_set_output_source(0, source);

    if (sending) {
        settings = obs_data_create();
        obs_data_set_string(settings, "url", argv[3]);
        obs_data_set_string(settings, "streamid", "robotweax-windows");
        const char* secret = getenv("ROBOTWEAX_OBS_PASSPHRASE");
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
        if (!obs_output_start(output))
            return 2;
        stage("output-started");
    }
    modules();
    char loaded[MAX_PATH];
    HMODULE provider = GetModuleHandleA("srt.dll");
    if (!provider || !GetModuleFileNameA(provider, loaded, sizeof(loaded))
        || _stricmp(loaded, argv[4]) != 0) {
        fprintf(stderr, "unexpected OBS SRT provider: %s\n", loaded);
        return 2;
    }
    fputs("READY\n", evidence);
    fflush(evidence);

    uint64_t start = GetTickCount64();
    uint64_t next_frame = start;
    unsigned frame_index = 0;
    bool good = true;
    while (GetTickCount64() - start < 30000) {
        uint64_t now = GetTickCount64();
        if (sending && now >= next_frame) {
            emit_synthetic(source, os_gettime_ns(), frame_index++);
            next_frame += 40;
        }
        if (command_ready()) {
            char command[80];
            if (!fgets(command, sizeof(command), stdin)
                || strncmp(command, "quit", 4) == 0)
                break;
        }
        if ((now - start) % 1000 < 20)
            report(output);
        Sleep(10);
    }
    report(output);
    modules();
    good = stop_output(output);
    obs_output_release(output);
    obs_encoder_release(vencoder);
    obs_encoder_release(aencoder);
    obs_service_release(service);
    obs_set_output_source(0, NULL);
    if (!sending)
        obs_source_remove_audio_capture_callback(source, inspect_audio, NULL);
    obs_source_filter_remove(source, filter);
    obs_source_release(filter);
    obs_source_release(source);
    obs_shutdown();
    fputs("SHUTDOWN\n", evidence);
    if (fclose(evidence))
        return 2;
    return good ? 0 : 1;
}
