#include "compat/platform_networking.hpp"

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

namespace robotweax::srt::compat {

int initialize_platform_networking() noexcept
{
#if defined(_WIN32)
    class WinsockRuntime {
    public:
        WinsockRuntime() noexcept
        {
            WSADATA data {};
            startup_error_ = WSAStartup(MAKEWORD(2, 2), &data);
        }

        ~WinsockRuntime()
        {
            if (startup_error_ == 0) {
                (void)WSACleanup();
            }
        }

        [[nodiscard]] int startup_error() const noexcept
        {
            return startup_error_;
        }

    private:
        int startup_error_ = 0;
    };

    static const WinsockRuntime runtime;
    return runtime.startup_error();
#else
    return 0;
#endif
}

} // namespace robotweax::srt::compat
