#include <srt/srt.h>

#if defined(ROBOTWEAX_SRT_EXPECT_AEAD_API_PREVIEW)                             \
    && !defined(ENABLE_AEAD_API_PREVIEW)
#error "the installed target did not propagate its AES-GCM API contract"
#endif

#ifdef ENABLE_AEAD_API_PREVIEW
_Static_assert(SRTO_CRYPTOMODE == 62, "AES-GCM socket option ABI");
_Static_assert(SRT_REJ_CRYPTO == 17, "AES-GCM rejection ABI");
#endif

int main(void)
{
    if (srt_startup() == SRT_ERROR) {
        return 1;
    }

    const int version = srt_getversion();
    const SRTSOCKET socket = srt_create_socket();
    if (socket == SRT_INVALID_SOCK) {
        (void)srt_cleanup();
        return 2;
    }

#ifdef ENABLE_AEAD_API_PREVIEW
    int crypto_mode = 2;
    if (srt_setsockflag(
            socket, SRTO_CRYPTOMODE, &crypto_mode, (int)sizeof(crypto_mode))
        == SRT_ERROR) {
        (void)srt_close(socket);
        (void)srt_cleanup();
        return 3;
    }
    crypto_mode = -1;
    int crypto_mode_size = (int)sizeof(crypto_mode);
    if (srt_getsockflag(
            socket, SRTO_CRYPTOMODE, &crypto_mode, &crypto_mode_size)
            == SRT_ERROR
        || crypto_mode != 2 || crypto_mode_size != (int)sizeof(crypto_mode)) {
        (void)srt_close(socket);
        (void)srt_cleanup();
        return 4;
    }
#endif

    const int close_result = srt_close(socket);
    const int cleanup_result = srt_cleanup();
    return version == SRT_VERSION_VALUE && close_result != SRT_ERROR
            && cleanup_result != SRT_ERROR
        ? 0
        : 5;
}
