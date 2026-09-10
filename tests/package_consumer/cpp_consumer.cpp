#include <srt/access_control.h>
#include <srt/srt.h>

#ifndef ROBOTWEAX_SRT_COMPAT_ACCESS_CONTROL_H
#error "access_control.h must come from the Robotweax installation"
#endif
static_assert(SRT_REJX_OVERLOAD == 1402);
static_assert(srt_logging::LogLevel::fatal == LOG_CRIT);
static_assert(srt_logging::LogLevel::error == LOG_ERR);
static_assert(srt_logging::LogLevel::warning == LOG_WARNING);
static_assert(srt_logging::LogLevel::note == LOG_NOTICE);
static_assert(srt_logging::LogLevel::debug == LOG_DEBUG);

#include <cstdint>
#include <stdexcept>

#if defined(ROBOTWEAX_SRT_EXPECT_AEAD_API_PREVIEW)                             \
    && !defined(ENABLE_AEAD_API_PREVIEW)
#error "the installed target did not propagate its AES-GCM API contract"
#endif

#ifdef ENABLE_AEAD_API_PREVIEW
static_assert(SRTO_CRYPTOMODE == 62);
static_assert(SRT_REJ_CRYPTO == 17);
#endif

namespace {

class Runtime final {
public:
    Runtime()
    {
        if (srt_startup() == SRT_ERROR) {
            throw std::runtime_error("srt_startup failed");
        }
    }

    ~Runtime()
    {
        (void)srt_cleanup();
    }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
};

} // namespace

int main()
{
    Runtime runtime;
    const srt_logging::LogLevel::type level = srt_logging::LogLevel::note;
    srt_setloglevel(level);
    const SRTSOCKET socket = srt_socket(AF_INET, SOCK_DGRAM, 0);
    if (socket == SRT_INVALID_SOCK) {
        return 1;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    std::int32_t crypto_mode = 2;
    if (srt_setsockflag(socket, SRTO_CRYPTOMODE, &crypto_mode,
            static_cast<int>(sizeof(crypto_mode)))
        == SRT_ERROR) {
        (void)srt_close(socket);
        return 2;
    }
    crypto_mode = -1;
    int crypto_mode_size = static_cast<int>(sizeof(crypto_mode));
    if (srt_getsockflag(
            socket, SRTO_CRYPTOMODE, &crypto_mode, &crypto_mode_size)
            == SRT_ERROR
        || crypto_mode != 2
        || crypto_mode_size != static_cast<int>(sizeof(crypto_mode))) {
        (void)srt_close(socket);
        return 3;
    }
#endif
    return srt_close(socket) == SRT_ERROR ? 4 : 0;
}
