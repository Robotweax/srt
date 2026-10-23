#include "robotweax/srt/payload_pool.hpp"

#include <new>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace robotweax::srt::detail {

PayloadStorage allocate_payload_storage(std::size_t bytes)
{
#if defined(__linux__) && defined(MADV_NOHUGEPAGE)
    // Small pools stay with the allocator. Large pools are mostly untouched at
    // low occupancy: promoting them to huge pages can make unused bytes resident.
    constexpr std::size_t minimum_mapping_bytes = 128U * 1024U;
    if (bytes >= minimum_mapping_bytes) {
        void* memory = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory != MAP_FAILED) {
            // Advise only pages owned by this reservation, never shared heap
            // pages. Unmap on rejection before falling back to ordinary storage.
            if (::madvise(memory, bytes, MADV_NOHUGEPAGE) == 0) {
                // Begin the byte-array lifetime without initializing its pages.
                return PayloadStorage {
                    ::new (memory) std::byte[bytes], {bytes}};
            }
            (void)::munmap(memory, bytes);
        }
    }
#endif
    // Advice is an optimization. Older kernels and restricted environments keep
    // the portable allocation behavior and its normal allocation-failure path.
    return PayloadStorage {new std::byte[bytes], {}};
}

void PayloadStorageDeleter::operator()(std::byte* memory) const noexcept
{
#if defined(__linux__)
    if (mapped_bytes != 0U) {
        (void)::munmap(memory, mapped_bytes);
        return;
    }
#endif
    delete[] memory;
}

} // namespace robotweax::srt::detail
