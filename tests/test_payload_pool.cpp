#include "test.hpp"
#include "robotweax/srt/payload_pool.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace robotweax::srt;

TEST(payload_pool_copy_move_and_reuse_preserve_independent_live_bytes)
{
    for (const std::size_t capacity : {1U, 128U, 4096U}) {
        detail::PayloadPool original {capacity};
        std::array<std::byte, maximum_data_payload_size> bytes;
        std::vector<std::uint32_t> indices;
        for (std::size_t index = 0; index < capacity; ++index) {
            bytes.fill(static_cast<std::byte>(index));
            indices.push_back(original.acquire(bytes));
        }
        for (std::size_t index = 0; index < capacity; index += 3) {
            original.release(indices[index]);
        }
        auto copied = original;
        detail::PayloadPool assigned {1};
        assigned = original;
        auto moved = std::move(copied);
        detail::PayloadPool move_assigned {128};
        move_assigned = std::move(assigned);
        bytes.fill(std::byte {0xab});
        for (std::size_t index = 0; index < capacity; index += 3) {
            const auto slot = original.acquire(bytes);
            REQUIRE_EQ(original.get(slot).back(), std::byte {0xab});
        }
        for (auto* independent : {&moved, &move_assigned}) {
            for (std::size_t index = 0; index < capacity; ++index) {
                if (index % 3 != 0) {
                    const auto data = independent->get(indices[index]);
                    REQUIRE_EQ(data.size(), maximum_data_payload_size);
                    for (const auto byte : data) {
                        REQUIRE_EQ(byte, static_cast<std::byte>(index));
                    }
                }
            }
            for (std::size_t index = 0; index < capacity; index += 3) {
                const auto slot = independent->acquire(bytes);
                REQUIRE_EQ(independent->get(slot).back(), std::byte {0xab});
            }
        }
        // Self-assignment retains the existing ownership and payloads.
        auto* self = &moved;
        moved = *self;
        REQUIRE_EQ(moved.get(indices.front()).back(), std::byte {0xab});
    }
}

TEST(payload_pool_rejects_invalid_reservations_before_allocation)
{
    for (const auto capacity :
        {std::size_t {0}, std::size_t {SequenceNumber::half_range},
            std::numeric_limits<std::size_t>::max()}) {
        bool rejected = false;
        try {
            detail::PayloadPool pool {capacity};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        REQUIRE(rejected);
    }
}

#if defined(__linux__) && defined(MADV_NOHUGEPAGE)
TEST(payload_pool_linux_mapping_policy_is_scoped_and_released)
{
    constexpr std::size_t bytes = 4U * 1024U * 1024U + 17U;
    // Probe the environment independently so a missing policy in the allocator
    // cannot silently turn this regression into a skip.
    void* probe = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    SKIP_UNLESS(probe != MAP_FAILED, "anonymous mapping unavailable");
    const bool advice_supported = ::madvise(probe, bytes, MADV_NOHUGEPAGE) == 0;
    REQUIRE_EQ(::munmap(probe, bytes), 0);
    SKIP_UNLESS(advice_supported, "MADV_NOHUGEPAGE unavailable");
    auto first = detail::allocate_payload_storage(bytes);
    REQUIRE_EQ(first.get_deleter().mapped_bytes, bytes);
    auto second = detail::allocate_payload_storage(bytes);
    REQUIRE_EQ(first.get_deleter().mapped_bytes, bytes);
    first[0] = std::byte {1};
    first[bytes - 1] = std::byte {2};
    second[bytes - 1] = std::byte {3};
    std::ifstream maps {"/proc/self/smaps"};
    SKIP_UNLESS(maps.is_open(), "proc smaps unavailable");
    const auto address = reinterpret_cast<std::uintptr_t>(first.get());
    bool contains = false;
    bool no_huge = false;
    std::string line;
    while (std::getline(maps, line)) {
        const auto dash = line.find('-');
        if (dash != std::string::npos && dash < line.find(' ')) {
            std::uintptr_t begin = 0, end = 0;
            char separator = 0;
            std::istringstream header {line};
            if (header >> std::hex >> begin >> separator >> end) {
                contains =
                    separator == '-' && address >= begin && address < end;
            }
        } else if (contains && line.starts_with("VmFlags:")) {
            std::istringstream flags {line.substr(8)};
            std::string flag;
            while (flags >> flag) {
                no_huge = no_huge || flag == "nh";
            }
            break;
        }
    }
    REQUIRE(no_huge);
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    REQUIRE(page_size > 0);
    REQUIRE_EQ(address % static_cast<std::uintptr_t>(page_size), 0U);
    auto* released = first.get();
    first.reset();
    unsigned char resident = 0;
    errno = 0;
    REQUIRE_EQ(
        ::mincore(released, static_cast<std::size_t>(page_size), &resident),
        -1);
    REQUIRE_EQ(errno, ENOMEM);
    REQUIRE_EQ(second[bytes - 1], std::byte {3});
}
#endif
