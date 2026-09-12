#include <srt/access_control.h>
#include <srt/srt.h>

#ifndef ROBOTWEAX_SRT_COMPAT_ACCESS_CONTROL_H
#error "access_control.h must come from the Robotweax installation"
#endif
static_assert(SRT_REJX_OVERLOAD == 1402);

#if defined(ROBOTWEAX_SRT_EXPECT_AEAD_API_PREVIEW)                             \
    && !defined(ENABLE_AEAD_API_PREVIEW)
#error "pkg-config did not propagate the AES-GCM API contract"
#endif

#ifdef ENABLE_AEAD_API_PREVIEW
static_assert(SRTO_CRYPTOMODE == 62);
static_assert(SRT_REJ_CRYPTO == 17);
#endif

int main()
{
    if (srt_startup() == SRT_ERROR) {
        return 1;
    }
    const int version = srt_getversion();
    const int cleanup_result = srt_cleanup();
    return version == SRT_VERSION_VALUE && cleanup_result != SRT_ERROR ? 0 : 2;
}
