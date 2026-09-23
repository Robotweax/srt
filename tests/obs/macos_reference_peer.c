// SPDX-License-Identifier: MIT
// Independent Haivision SRT peer. Never load into the OBS test process.
#include <srt/srt.h>
#include "reference_pacing.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void fail(const char* message)
{
    fprintf(stderr, "%s: %s\n", message, srt_getlasterror_str());
    exit(2);
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void option(
    SRTSOCKET socket, SRT_SOCKOPT name, const void* value, int length)
{
    if (srt_setsockflag(socket, name, value, length) == SRT_ERROR)
        fail("cannot set reference socket option");
}

static SRTSOCKET connect_peer(
    bool listener, bool sender, unsigned port, const char* passphrase)
{
    SRTSOCKET socket = srt_create_socket();
    if (socket == SRT_INVALID_SOCK)
        fail("cannot create reference socket");
    int live = SRTT_LIVE, timeout = 5000, yes = sender ? 1 : 0;
    option(socket, SRTO_TRANSTYPE, &live, sizeof(live));
    option(socket, SRTO_SENDER, &yes, sizeof(yes));
    option(socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout));
    option(socket, SRTO_SNDTIMEO, &timeout, sizeof(timeout));
    if (strcmp(passphrase, "-") != 0) {
        int key_length = 16;
        option(socket, SRTO_PASSPHRASE, passphrase, (int)strlen(passphrase));
        option(socket, SRTO_PBKEYLEN, &key_length, sizeof(key_length));
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener) {
        if (srt_bind(socket, (const struct sockaddr*)&address, sizeof(address))
                == SRT_ERROR
            || srt_listen(socket, 1) == SRT_ERROR)
            fail("cannot listen with reference socket");
        puts("LISTENING");
        fflush(stdout);
        SRTSOCKET accepted = srt_accept(socket, NULL, NULL);
        srt_close(socket);
        if (accepted == SRT_INVALID_SOCK)
            fail("cannot accept reference connection");
        return accepted;
    }
    if (srt_connect(socket, (const struct sockaddr*)&address, sizeof(address))
        == SRT_ERROR)
        fail("cannot connect reference socket");
    return socket;
}

static uint64_t transfer(
    SRTSOCKET socket, bool sender, const char* path, uint64_t receive_limit)
{
    FILE* file = fopen(path, sender ? "rb" : "wb");
    if (!file) {
        perror(path);
        exit(2);
    }
    char buffer[2048];
    uint64_t total = 0;
    uint64_t next_progress = 65536;
    uint64_t started = monotonic_ns(), send_ns = 0, sleep_ns = 0;
    uint64_t requested_sleep_ns = 0, packets = 0;
    struct obs_reference_pacer pacer = obs_reference_pacer_start(started);
    for (;;) {
        int size;
        if (sender) {
            size_t read_size = fread(buffer, 1, 1316, file);
            if (!read_size)
                break;
            uint64_t wait_ns;
            while ((wait_ns = obs_reference_pacer_delay(&pacer, monotonic_ns()))
                != 0) {
                struct timespec delay = {
                    .tv_sec = (time_t)(wait_ns / UINT64_C(1000000000)),
                    .tv_nsec = (long)(wait_ns % UINT64_C(1000000000))};
                uint64_t before = monotonic_ns();
                if (nanosleep(&delay, NULL) && errno != EINTR) {
                    perror("nanosleep");
                    exit(2);
                }
                sleep_ns += monotonic_ns() - before;
                requested_sleep_ns += wait_ns;
            }
            uint64_t before = monotonic_ns();
            size = srt_sendmsg(socket, buffer, (int)read_size, -1, 1);
            send_ns += monotonic_ns() - before;
            if (size != (int)read_size)
                fail("cannot send reference payload");
            ++packets;
        } else {
            size = srt_recvmsg(socket, buffer, sizeof(buffer));
            if (size == SRT_ERROR)
                break;
            if (size > 0
                && fwrite(buffer, 1, (size_t)size, file) != (size_t)size) {
                perror("cannot write reference output");
                exit(2);
            }
            fflush(file);
        }
        total += (uint64_t)size;
        if (sender && total >= next_progress) {
            printf("SENT %llu\n", (unsigned long long)total);
            printf("PACING elapsed_ns=%llu send_ns=%llu sleep_ns=%llu "
                   "requested_sleep_ns=%llu\n",
                (unsigned long long)(monotonic_ns() - started),
                (unsigned long long)send_ns, (unsigned long long)sleep_ns,
                (unsigned long long)requested_sleep_ns);
            fflush(stdout);
            next_progress += 65536;
        }
        if (!sender && total >= receive_limit)
            break;
    }
    if (sender)
        printf("PACING packets=%llu elapsed_ns=%llu send_ns=%llu "
               "sleep_ns=%llu requested_sleep_ns=%llu\n",
            (unsigned long long)packets,
            (unsigned long long)(monotonic_ns() - started),
            (unsigned long long)send_ns, (unsigned long long)sleep_ns,
            (unsigned long long)requested_sleep_ns);
    fclose(file);
    return total;
}

int main(int argc, char** argv)
{
    if (argc != 7 && argc != 8) {
        fprintf(stderr,
            "usage: peer send|receive caller|listener PORT KEY FILE "
            "EXPECTED_DYLIB [RECEIVE_LIMIT_BYTES]\n");
        return 2;
    }
    bool sender = strcmp(argv[1], "send") == 0;
    bool receiver = strcmp(argv[1], "receive") == 0;
    bool listener = strcmp(argv[2], "listener") == 0;
    unsigned port = (unsigned)strtoul(argv[3], NULL, 10);
    if ((!sender && !receiver) || (!listener && strcmp(argv[2], "caller") != 0)
        || port == 0 || port > UINT16_MAX || (sender && argc == 8))
        return 2;
    uint64_t receive_limit = 200000;
    if (argc == 8) {
        char* end = NULL;
        errno = 0;
        unsigned long long parsed = strtoull(argv[7], &end, 10);
        if (errno || !end || *end || parsed < 200000 || parsed > 100000000)
            return 2;
        receive_limit = (uint64_t)parsed;
    }
    if (srt_startup() == SRT_ERROR)
        fail("reference startup failed");
    Dl_info info;
    if (!dladdr((const void*)&srt_startup, &info)) {
        fprintf(stderr, "cannot identify reference provider\n");
        return 2;
    }
    char* loaded = realpath(info.dli_fname, NULL);
    char* expected = realpath(argv[6], NULL);
    bool correct_provider = loaded && expected && strcmp(loaded, expected) == 0;
    free(loaded);
    free(expected);
    if (!correct_provider) {
        fprintf(stderr, "unexpected reference provider: %s\n", info.dli_fname);
        return 2;
    }
    printf("PROVIDER %s\n", info.dli_fname);
    fflush(stdout);
    SRTSOCKET socket = connect_peer(listener, sender, port, argv[4]);
    uint64_t total = transfer(socket, sender, argv[5], receive_limit);
    if (sender) {
        char command[16];
        printf("QUEUED %llu\n", (unsigned long long)total);
        fflush(stdout);
        if (!fgets(command, sizeof(command), stdin)
            || strncmp(command, "quit", 4) != 0)
            return 2;
    }
    srt_close(socket);
    srt_cleanup();
    printf("BYTES %llu\n", (unsigned long long)total);
    return total ? 0 : 1;
}
