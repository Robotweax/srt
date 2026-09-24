#include <srt/srt.h>

int main()
{
    if (srt_startup() != 0) {
        return 1;
    }
    const auto version = srt_getversion();
    return srt_cleanup() == 0 && version != 0 ? 0 : 2;
}
