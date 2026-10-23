#include "test.hpp"
#include "robotweax/srt/payload_pool.hpp"

#include <cerrno>
#include <cstddef>
#include <sys/mman.h>

namespace {
bool reject_mapping = false;
bool reject_advice = false;
unsigned mappings = 0;
unsigned advice_calls = 0;
unsigned unmaps = 0;

void reset(bool mapping_failure, bool advice_failure)
{
    reject_mapping = mapping_failure;
    reject_advice = advice_failure;
    mappings = advice_calls = unmaps = 0;
}
} // namespace

extern "C" void* __real_mmap(void*, std::size_t, int, int, int, off_t);
extern "C" int __real_madvise(void*, std::size_t, int);
extern "C" int __real_munmap(void*, std::size_t);

extern "C" void* __wrap_mmap(void* address, std::size_t bytes, int protection,
    int flags, int descriptor, off_t offset)
{
    ++mappings;
    if (reject_mapping) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return __real_mmap(address, bytes, protection, flags, descriptor, offset);
}
extern "C" int __wrap_madvise(void* address, std::size_t bytes, int advice)
{
    ++advice_calls;
    if (reject_advice) {
        errno = EINVAL;
        return -1;
    }
    return __real_madvise(address, bytes, advice);
}
extern "C" int __wrap_munmap(void* address, std::size_t bytes)
{
    ++unmaps;
    return __real_munmap(address, bytes);
}

TEST(payload_storage_small_pool_does_not_request_mapping)
{
    reset(false, false);
    auto storage = robotweax::srt::detail::allocate_payload_storage(1456);
    REQUIRE_EQ(storage.get_deleter().mapped_bytes, 0U);
    storage[1455] = std::byte {7};
    storage.reset();
    REQUIRE_EQ(mappings, 0U);
    REQUIRE_EQ(advice_calls, 0U);
    REQUIRE_EQ(unmaps, 0U);
}

#if defined(MADV_NOHUGEPAGE)
TEST(payload_storage_mapping_failure_falls_back_to_ordinary_owned_storage)
{
    reset(true, false);
    constexpr std::size_t bytes = 4U * 1024U * 1024U + 17U;
    auto storage = robotweax::srt::detail::allocate_payload_storage(bytes);
    REQUIRE_EQ(storage.get_deleter().mapped_bytes, 0U);
    storage[0] = std::byte {1};
    storage[bytes - 1] = std::byte {2};
    REQUIRE_EQ(storage[0], std::byte {1});
    REQUIRE_EQ(storage[bytes - 1], std::byte {2});
    storage.reset();
    REQUIRE_EQ(mappings, 1U);
    REQUIRE_EQ(advice_calls, 0U);
    REQUIRE_EQ(unmaps, 0U);
}

TEST(payload_storage_advice_failure_releases_mapping_before_fallback)
{
    reset(false, true);
    constexpr std::size_t bytes = 4U * 1024U * 1024U + 17U;
    auto storage = robotweax::srt::detail::allocate_payload_storage(bytes);
    REQUIRE_EQ(storage.get_deleter().mapped_bytes, 0U);
    REQUIRE_EQ(mappings, 1U);
    REQUIRE_EQ(advice_calls, 1U);
    REQUIRE_EQ(unmaps, 1U);
    storage[bytes - 1] = std::byte {3};
    storage.reset();
    REQUIRE_EQ(unmaps, 1U);
}
#endif
