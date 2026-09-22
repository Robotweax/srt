// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <srt/srt.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char* message)
{
    fprintf(stderr, "%s: %s\n", message, srt_getlasterror_str());
    exit(2);
}

static void provider(void)
{
    HMODULE module = GetModuleHandleA("srt.dll");
    char path[MAX_PATH];
    if (!module || !GetModuleFileNameA(module, path, sizeof(path))) {
        fprintf(stderr, "cannot identify reference SRT provider\n");
        exit(2);
    }
    printf("PROVIDER %s\n", path);
    fflush(stdout);
}

static void configure(SRTSOCKET socket, bool sender, const char* passphrase)
{
    int live = SRTT_LIVE;
    int timeout = 5000;
    int yes = sender ? 1 : 0;
    if (srt_setsockflag(socket, SRTO_TRANSTYPE, &live, sizeof(live))
            == SRT_ERROR
        || srt_setsockflag(socket, SRTO_SENDER, &yes, sizeof(yes)) == SRT_ERROR
        || srt_setsockflag(socket, SRTO_RCVTIMEO, &timeout, sizeof(timeout))
            == SRT_ERROR
        || srt_setsockflag(socket, SRTO_SNDTIMEO, &timeout, sizeof(timeout))
            == SRT_ERROR)
        fail("cannot configure reference socket");
    if (strcmp(passphrase, "-") != 0) {
        int key_length = 16;
        if (srt_setsockflag(
                socket, SRTO_PASSPHRASE, passphrase, (int)strlen(passphrase))
                == SRT_ERROR
            || srt_setsockflag(
                   socket, SRTO_PBKEYLEN, &key_length, sizeof(key_length))
                == SRT_ERROR)
            fail("cannot configure reference encryption");
    }
}

static SRTSOCKET connect_socket(
    bool listener, bool sender, uint16_t port, const char* passphrase)
{
    SRTSOCKET socket = srt_create_socket();
    if (socket == SRT_INVALID_SOCK)
        fail("cannot create reference socket");
    configure(socket, sender, passphrase);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
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

static uint64_t receive_file(SRTSOCKET socket, const char* path)
{
    FILE* output = fopen(path, "wb");
    if (!output) {
        perror("cannot create reference output");
        exit(2);
    }
    uint8_t buffer[2048];
    uint64_t total = 0;
    for (;;) {
        int size = srt_recvmsg(socket, (char*)buffer, sizeof(buffer));
        if (size == SRT_ERROR)
            break;
        if (size > 0
            && fwrite(buffer, 1, (size_t)size, output) != (size_t)size) {
            perror("cannot write reference output");
            exit(2);
        }
        total += (uint64_t)size;
        fflush(output);
    }
    fclose(output);
    return total;
}

static uint64_t send_file(SRTSOCKET socket, const char* path)
{
    FILE* input = fopen(path, "rb");
    if (!input) {
        perror("cannot open reference input");
        exit(2);
    }
    uint8_t buffer[1316];
    uint64_t total = 0;
    for (;;) {
        size_t size = fread(buffer, 1, sizeof(buffer), input);
        if (!size)
            break;
        int sent = srt_sendmsg(socket, (const char*)buffer, (int)size, -1, 1);
        if (sent != (int)size)
            fail("cannot send reference payload");
        total += size;
        /* Replay the captured transport stream as live media, not a burst. */
        Sleep(15);
    }
    fclose(input);
    return total;
}

int main(int argc, char** argv)
{
    if (argc != 7) {
        fprintf(stderr,
            "usage: reference-peer send|receive caller|listener PORT "
            "PASSPHRASE INPUT_OR_OUTPUT EXPECTED_PROVIDER\n");
        return 2;
    }
    bool sender = strcmp(argv[1], "send") == 0;
    bool receiver = strcmp(argv[1], "receive") == 0;
    bool listener = strcmp(argv[2], "listener") == 0;
    if ((!sender && !receiver) || (!listener && strcmp(argv[2], "caller") != 0))
        return 2;
    unsigned long parsed_port = strtoul(argv[3], NULL, 10);
    if (!parsed_port || parsed_port > UINT16_MAX)
        return 2;
    if (srt_startup() == SRT_ERROR)
        fail("reference startup failed");
    provider();
    char loaded[MAX_PATH];
    HMODULE module = GetModuleHandleA("srt.dll");
    if (!module || !GetModuleFileNameA(module, loaded, sizeof(loaded))
        || _stricmp(loaded, argv[6]) != 0) {
        fprintf(stderr, "unexpected reference provider: %s\n", loaded);
        return 2;
    }
    SRTSOCKET socket =
        connect_socket(listener, sender, (uint16_t)parsed_port, argv[4]);
    uint64_t total =
        sender ? send_file(socket, argv[5]) : receive_file(socket, argv[5]);
    if (sender) {
        /* Keep the connection open until OBS has observed decoded media. */
        char command[16];
        printf("QUEUED %llu\n", (unsigned long long)total);
        fflush(stdout);
        if (!fgets(command, sizeof(command), stdin)
            || strncmp(command, "quit", 4) != 0) {
            fprintf(stderr, "reference sender did not receive quit command\n");
            return 2;
        }
    }
    srt_close(socket);
    srt_cleanup();
    printf("BYTES %llu\n", (unsigned long long)total);
    return total ? 0 : 1;
}
