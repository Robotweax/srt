/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <gmodule.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gboolean verify_provider(void)
{
    const gchar* expected_plugin = g_getenv("ROBOTWEAX_GST_PLUGIN");
    const gchar* expected_library = g_getenv("ROBOTWEAX_SRT_LIBRARY_DIR");
    GstElementFactory* factory = gst_element_factory_find("srtsrc");
    if (factory == NULL || expected_plugin == NULL
        || expected_library == NULL) {
        g_printerr("missing SRT plugin or provider verification environment\n");
        if (factory != NULL) {
            gst_object_unref(factory);
        }
        return FALSE;
    }
    GstPlugin* plugin =
        gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory));
    const gchar* filename =
        plugin != NULL ? gst_plugin_get_filename(plugin) : NULL;
    gboolean valid =
        filename != NULL && g_strcmp0(filename, expected_plugin) == 0;
    GModule* module = valid
        ? g_module_open(filename, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL)
        : NULL;
    gpointer symbol = NULL;
    Dl_info info;
    if (module == NULL || !g_module_symbol(module, "srt_startup", &symbol)
        || dladdr(symbol, &info) == 0) {
        valid = FALSE;
    } else {
        char* actual = realpath(info.dli_fname, NULL);
        gchar* parent = actual != NULL ? g_path_get_dirname(actual) : NULL;
        valid = g_strcmp0(parent, expected_library) == 0;
        g_print("PROVIDER plugin=%s library=%s\n", filename,
            actual != NULL ? actual : "unresolved");
        g_free(parent);
        free(actual);
    }
    if (module != NULL) {
        g_module_close(module);
    }
    if (plugin != NULL) {
        gst_object_unref(plugin);
    }
    gst_object_unref(factory);
    if (!valid) {
        g_printerr("unexpected GStreamer plugin or SRT runtime provider\n");
    }
    return valid;
}

static gboolean allow_caller(GstElement* element, gpointer address,
    const gchar* stream_id, gpointer user_data)
{
    (void)element;
    (void)address;
    (void)user_data;
    gboolean allowed = g_strcmp0(stream_id, "robotweax-test") == 0;
    g_print("CALLER %s %s\n", allowed ? "accepted" : "rejected",
        stream_id != NULL ? stream_id : "(none)");
    fflush(stdout);
    return allowed;
}

static gboolean bus_failed(GstBus* bus)
{
    GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (message == NULL) {
        return FALSE;
    }
    GError* error = NULL;
    gchar* detail = NULL;
    gst_message_parse_error(message, &error, &detail);
    g_printerr("pipeline error: %s (%s)\n", error->message,
        detail != NULL ? detail : "");
    g_clear_error(&error);
    g_free(detail);
    gst_message_unref(message);
    return TRUE;
}

static guint64 bytes_stat(GstElement* transport, gboolean sending)
{
    GstStructure* stats = NULL;
    guint64 bytes = 0;
    g_object_get(transport, "stats", &stats, NULL);
    if (stats != NULL) {
        gst_structure_get_uint64(stats,
            sending ? "bytes-sent-total" : "bytes-received-total", &bytes);
        gst_structure_free(stats);
    }
    return bytes;
}

int main(int argc, char** argv)
{
    gst_init(&argc, &argv);
    if (!verify_provider()) {
        return 1;
    }
    guint64 timeout_seconds = 20;
    const gchar* timeout = g_getenv("ROBOTWEAX_TEST_PEER_TIMEOUT_SECONDS");
    if (timeout != NULL
        && !g_ascii_string_to_unsigned(
            timeout, 10, 1, 86500, &timeout_seconds, NULL)) {
        g_printerr("invalid test peer timeout\n");
        return 1;
    }
    if (argc != 5) {
        g_printerr(
            "usage: peer send|receive|receive-repeat|stop-source|stop-sink URI "
            "FILE BYTE_LIMIT\n");
        return 2;
    }
    const gboolean sending =
        strcmp(argv[1], "send") == 0 || strcmp(argv[1], "stop-sink") == 0;
    const gboolean stopping = g_str_has_prefix(argv[1], "stop-");
    const gboolean repeating = strcmp(argv[1], "receive-repeat") == 0;
    if (!sending && !stopping && !repeating
        && strcmp(argv[1], "receive") != 0) {
        return 2;
    }
    guint64 byte_limit = g_ascii_strtoull(argv[4], NULL, 10);
    GError* error = NULL;
    GstElement* pipeline = gst_parse_launch(sending
            ? "filesrc name=input blocksize=1316 ! identity sleep-time=5000 ! "
              "srtsink name=transport sync=false"
            : "srtsrc name=transport ! appsink name=output sync=false "
              "max-buffers=64",
        &error);
    if (error != NULL || pipeline == NULL) {
        g_printerr("cannot create pipeline: %s\n",
            error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        if (pipeline != NULL) {
            gst_object_unref(pipeline);
        }
        return 1;
    }
    GstElement* transport = gst_bin_get_by_name(GST_BIN(pipeline), "transport");
    GstElement* endpoint =
        gst_bin_get_by_name(GST_BIN(pipeline), sending ? "input" : "output");
    GstBus* bus = gst_element_get_bus(pipeline);
    g_object_set(transport, "uri", argv[2], "latency", 80, "poll-timeout", 50,
        "auto-reconnect", FALSE, NULL);
    if (strstr(argv[2], "mode=listener") != NULL) {
        g_object_set(transport, "authentication", TRUE, NULL);
        g_signal_connect(
            transport, "caller-connecting", G_CALLBACK(allow_caller), NULL);
    }
    if (sending) {
        g_object_set(endpoint, "location", argv[3], NULL);
    } else {
        g_object_set(transport, "keep-listening", repeating, NULL);
    }

    int result = 1;
    FILE* output = NULL;
    if (stopping) {
        for (int iteration = 0; iteration < 5; ++iteration) {
            if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
                == GST_STATE_CHANGE_FAILURE) {
                g_printerr(
                    "pipeline restart failed at cycle %d\n", iteration + 1);
                bus_failed(bus);
                goto cleanup;
            }
            gst_element_get_state(pipeline, NULL, NULL, 100 * GST_MSECOND);
            if (bus_failed(bus)) {
                goto cleanup;
            }
            gint64 started = g_get_monotonic_time();
            if (gst_element_set_state(pipeline, GST_STATE_NULL)
                    == GST_STATE_CHANGE_FAILURE
                || g_get_monotonic_time() - started > 2000000) {
                g_printerr("pipeline stop exceeded two seconds\n");
                goto cleanup;
            }
        }
        g_print("STOP cycles=5\n");
        result = 0;
        goto cleanup;
    }
    if (!sending) {
        output = fopen(argv[3], "wb");
        if (output == NULL) {
            perror("fopen");
            goto cleanup;
        }
    }
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE) {
        bus_failed(bus);
        goto cleanup;
    }
    g_print("STARTED\n");
    fflush(stdout);

    guint64 total = 0;
    guint64 stats_bytes = 0;
    const gint64 deadline =
        g_get_monotonic_time() + (gint64)timeout_seconds * G_USEC_PER_SEC;
    gboolean finished = FALSE;
    while (g_get_monotonic_time() < deadline) {
        if (sending) {
            GstMessage* message = gst_bus_timed_pop_filtered(
                bus, 50 * GST_MSECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
            if (message != NULL) {
                gboolean eos = GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
                if (!eos) {
                    gst_bus_post(bus, gst_message_ref(message));
                    bus_failed(bus);
                }
                gst_message_unref(message);
                finished = eos;
                break;
            }
        } else {
            GstSample* sample = gst_app_sink_try_pull_sample(
                GST_APP_SINK(endpoint), 50 * GST_MSECOND);
            if (sample != NULL) {
                GstMapInfo mapping;
                GstBuffer* buffer = gst_sample_get_buffer(sample);
                if (!gst_buffer_map(buffer, &mapping, GST_MAP_READ)) {
                    gst_sample_unref(sample);
                    goto cleanup;
                }
                size_t wanted = mapping.size;
                if (byte_limit != 0) {
                    wanted = MIN((guint64)wanted, byte_limit - total);
                }
                size_t written = fwrite(mapping.data, 1, wanted, output);
                fflush(output);
                total += written;
                gboolean complete_write = written == wanted;
                gst_buffer_unmap(buffer, &mapping);
                gst_sample_unref(sample);
                if (!complete_write) {
                    goto cleanup;
                }
                stats_bytes = MAX(stats_bytes, bytes_stat(transport, FALSE));
                if (byte_limit != 0 && total >= byte_limit) {
                    finished = TRUE;
                    break;
                }
            } else if (gst_app_sink_is_eos(GST_APP_SINK(endpoint))) {
                finished = TRUE;
                break;
            }
        }
        if (bus_failed(bus)) {
            goto cleanup;
        }
    }
    stats_bytes = MAX(stats_bytes, bytes_stat(transport, sending));
    if (!finished || bus_failed(bus) || stats_bytes == 0) {
        g_printerr("incomplete transfer or missing statistics: finished=%d "
                   "stats=%" G_GUINT64_FORMAT "\n",
            finished, stats_bytes);
        goto cleanup;
    }
    if (!sending && byte_limit != 0 && total != byte_limit) {
        g_printerr("unexpected receive size: %" G_GUINT64_FORMAT
                   " (expected %" G_GUINT64_FORMAT ")\n",
            total, byte_limit);
        goto cleanup;
    }
    g_print("DONE bytes=%" G_GUINT64_FORMAT " stats=%" G_GUINT64_FORMAT "\n",
        total, stats_bytes);
    fflush(stdout);
    if (sending) {
        /* Gst EOS is not an end-to-end SRT delivery receipt. Keep the live
         * connection open until the controller acknowledges the receiver. */
        if (getchar() != '\n') {
            g_printerr("missing controlled shutdown acknowledgement\n");
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (output != NULL && fclose(output) != 0) {
        result = 1;
    }
    gst_object_unref(bus);
    gst_object_unref(endpoint);
    gst_object_unref(transport);
    gst_object_unref(pipeline);
    gst_deinit();
    return result;
}
