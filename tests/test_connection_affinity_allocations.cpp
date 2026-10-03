// Allocation probe belongs only to the isolated ownership-model executable.
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace robotweax::srt::test {
std::atomic<std::size_t> affinity_allocations {0};
}

void* operator new(std::size_t size)
{
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) {
        ++robotweax::srt::test::affinity_allocations;
        return memory;
    }
    throw std::bad_alloc {};
}
void* operator new[](std::size_t size)
{
    return ::operator new(size);
}
void operator delete(void* memory) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory) noexcept
{
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    void* memory = nullptr;
#ifdef _WIN32
    memory = _aligned_malloc(
        size == 0 ? 1 : size, static_cast<std::size_t>(alignment));
    if (memory == nullptr)
        throw std::bad_alloc {};
#else
    if (posix_memalign(
            &memory, static_cast<std::size_t>(alignment), size == 0 ? 1 : size)
        != 0)
        throw std::bad_alloc {};
#endif
    ++robotweax::srt::test::affinity_allocations;
    return memory;
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept
{
#ifdef _WIN32
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}
void operator delete[](void* memory, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}
void operator delete(
    void* memory, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}
void operator delete[](
    void* memory, std::size_t, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}
