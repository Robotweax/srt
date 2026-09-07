#include <srt/srt.h>

int main(void)
{
    if (SRT_VERSION_VALUE < SRT_MAKE_VERSION(1, 3, 0)) {
        return 1;
    }
    if (srt_startup() == SRT_ERROR) {
        return 2;
    }

    const SRTSOCKET socket = srt_socket(AF_INET, SOCK_DGRAM, 0);
    if (socket == SRT_INVALID_SOCK) {
        (void)srt_cleanup();
        return 3;
    }

    (void)srt_close(socket);
    return srt_cleanup() == SRT_ERROR ? 4 : 0;
}
