#include <srt/access_control.h>
#include <srt/srt.h>

#ifndef ROBOTWEAX_SRT_COMPAT_ACCESS_CONTROL_H
#error "access_control.h must come from the Robotweax installation"
#endif
_Static_assert(SRT_REJX_FALLBACK == 1000, "SRT_REJX_FALLBACK");
_Static_assert(SRT_REJX_KEY_NOTSUP == 1001, "SRT_REJX_KEY_NOTSUP");
_Static_assert(SRT_REJX_FILEPATH == 1002, "SRT_REJX_FILEPATH");
_Static_assert(SRT_REJX_HOSTNOTFOUND == 1003, "SRT_REJX_HOSTNOTFOUND");
_Static_assert(SRT_REJX_BAD_REQUEST == 1400, "SRT_REJX_BAD_REQUEST");
_Static_assert(SRT_REJX_UNAUTHORIZED == 1401, "SRT_REJX_UNAUTHORIZED");
_Static_assert(SRT_REJX_OVERLOAD == 1402, "SRT_REJX_OVERLOAD");
_Static_assert(SRT_REJX_FORBIDDEN == 1403, "SRT_REJX_FORBIDDEN");
_Static_assert(SRT_REJX_NOTFOUND == 1404, "SRT_REJX_NOTFOUND");
_Static_assert(SRT_REJX_BAD_MODE == 1405, "SRT_REJX_BAD_MODE");
_Static_assert(SRT_REJX_UNACCEPTABLE == 1406, "SRT_REJX_UNACCEPTABLE");
_Static_assert(SRT_REJX_CONFLICT == 1409, "SRT_REJX_CONFLICT");
_Static_assert(SRT_REJX_NOTSUP_MEDIA == 1415, "SRT_REJX_NOTSUP_MEDIA");
_Static_assert(SRT_REJX_LOCKED == 1423, "SRT_REJX_LOCKED");
_Static_assert(SRT_REJX_FAILED_DEPEND == 1424, "SRT_REJX_FAILED_DEPEND");
_Static_assert(SRT_REJX_ISE == 1500, "SRT_REJX_ISE");
_Static_assert(SRT_REJX_UNIMPLEMENTED == 1501, "SRT_REJX_UNIMPLEMENTED");
_Static_assert(SRT_REJX_GW == 1502, "SRT_REJX_GW");
_Static_assert(SRT_REJX_DOWN == 1503, "SRT_REJX_DOWN");
_Static_assert(SRT_REJX_VERSION == 1505, "SRT_REJX_VERSION");
_Static_assert(SRT_REJX_NOROOM == 1507, "SRT_REJX_NOROOM");

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

    int32_t implementation_version = 0;
    int implementation_size = (int)sizeof(implementation_version);
    if (srt_getsockflag(socket, SRTO_ROBOTWEAX_VERSION, &implementation_version,
            &implementation_size)
            == SRT_ERROR
        || implementation_size != (int)sizeof(implementation_version)
        || implementation_version != ROBOTWEAX_SRT_VERSION_VALUE) {
        (void)srt_close(socket);
        (void)srt_cleanup();
        return 10;
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
