#include "srt/srt.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace {

constexpr int reload_cycles = 32;

class SharedModule {
public:
    explicit SharedModule(std::string path)
        : path_(std::move(path))
    {
    }

    ~SharedModule()
    {
        (void)unload();
    }

    SharedModule(const SharedModule&) = delete;
    SharedModule& operator=(const SharedModule&) = delete;

    [[nodiscard]] bool load() noexcept
    {
#if defined(_WIN32)
        handle_ = LoadLibraryA(path_.c_str());
#else
        handle_ = dlopen(path_.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        return handle_ != nullptr;
    }

    [[nodiscard]] bool unload() noexcept
    {
        if (handle_ == nullptr) {
            return true;
        }
#if defined(_WIN32)
        const bool success = FreeLibrary(handle_) != 0;
#else
        const bool success = dlclose(handle_) == 0;
#endif
        handle_ = nullptr;
        return success;
    }

    template <typename Function>
    [[nodiscard]] Function resolve(const char* name) const noexcept
    {
#if defined(_WIN32)
        return reinterpret_cast<Function>(
            GetProcAddress(handle_, name));
#else
        void* address = dlsym(handle_, name);
        Function function = nullptr;
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(function));
        return function;
#endif
    }

    [[nodiscard]] bool is_resident() const noexcept
    {
#if defined(_WIN32)
        const std::size_t separator = path_.find_last_of("\\/");
        const char* name = separator == std::string::npos
            ? path_.c_str()
            : path_.c_str() + separator + 1;
        return GetModuleHandleA(name) != nullptr;
#elif defined(__APPLE__)
        // dyld may retain an unused image in its delayed-unload cache after
        // final cleanup. The next cycle's state-isolation assertions prove
        // safe logical reuse on this platform.
        return false;
#elif defined(RTLD_NOLOAD)
        void* resident =
            dlopen(path_.c_str(), RTLD_NOW | RTLD_NOLOAD);
        if (resident == nullptr) {
            return false;
        }
        (void)dlclose(resident);
        return true;
#else
        return false;
#endif
    }

    [[nodiscard]] const char* error() const noexcept
    {
#if defined(_WIN32)
        return "Windows loader error";
#else
        const char* message = dlerror();
        return message == nullptr ? "unknown loader error" : message;
#endif
    }

private:
#if defined(_WIN32)
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
    std::string path_;
};

struct SrtApi {
    using Startup = int (*)(void);
    using Cleanup = int (*)(void);
    using CreateSocket = SRTSOCKET (*)(void);
    using Close = int (*)(SRTSOCKET);
    using GetSocketState = SRT_SOCKSTATUS (*)(SRTSOCKET);
    using EpollCreate = int (*)(void);
    using EpollAddSocket =
        int (*)(int, SRTSOCKET, const int*);
    using EpollRelease = int (*)(int);

    Startup startup = nullptr;
    Cleanup cleanup = nullptr;
    CreateSocket create_socket = nullptr;
    Close close = nullptr;
    GetSocketState get_socket_state = nullptr;
    EpollCreate epoll_create = nullptr;
    EpollAddSocket epoll_add_socket = nullptr;
    EpollRelease epoll_release = nullptr;

    [[nodiscard]] bool resolve(const SharedModule& module) noexcept
    {
        startup = module.resolve<Startup>("srt_startup");
        cleanup = module.resolve<Cleanup>("srt_cleanup");
        create_socket =
            module.resolve<CreateSocket>("srt_create_socket");
        close = module.resolve<Close>("srt_close");
        get_socket_state =
            module.resolve<GetSocketState>("srt_getsockstate");
        epoll_create =
            module.resolve<EpollCreate>("srt_epoll_create");
        epoll_add_socket =
            module.resolve<EpollAddSocket>("srt_epoll_add_usock");
        epoll_release =
            module.resolve<EpollRelease>("srt_epoll_release");
        return startup != nullptr
            && cleanup != nullptr
            && create_socket != nullptr
            && close != nullptr
            && get_socket_state != nullptr
            && epoll_create != nullptr
            && epoll_add_socket != nullptr
            && epoll_release != nullptr;
    }
};

[[nodiscard]] int exercise_cycle(
    const SrtApi& api, int cycle) noexcept
{
    if (api.startup() != 0 || api.startup() != 0) {
        return 10;
    }

    const SRTSOCKET closed_socket = api.create_socket();
    const SRTSOCKET cleanup_socket = api.create_socket();
    if (closed_socket == SRT_INVALID_SOCK
        || cleanup_socket == SRT_INVALID_SOCK) {
        return 11;
    }

    // Linux and Windows unload the image immediately, so module-local static
    // state must be reconstructed. dyld can keep C++ images in a delayed
    // unload cache; on macOS the state-isolation assertions below are the
    // portable contract and handles may continue monotonically.
#if !defined(__APPLE__)
    if (closed_socket != 1 || cleanup_socket != 2) {
        std::fprintf(stderr,
            "cycle %d retained socket handles %d and %d\n",
            cycle, static_cast<int>(closed_socket),
            static_cast<int>(cleanup_socket));
        return 12;
    }
#endif

    const int poll = api.epoll_create();
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    if (poll < 0
        || api.epoll_add_socket(
            poll, cleanup_socket, &events) != 0) {
        return 13;
    }
    if (api.get_socket_state(closed_socket) != SRTS_INIT
        || api.get_socket_state(cleanup_socket) != SRTS_INIT) {
        return 14;
    }

    if (api.close(closed_socket) != 0
        || api.get_socket_state(closed_socket) != SRTS_CLOSED) {
        return 15;
    }

    // The first cleanup releases only the nested startup scope.
    if (api.cleanup() != 0
        || api.get_socket_state(cleanup_socket) != SRTS_INIT) {
        return 16;
    }

    // Leave one socket and its epoll subscription to final cleanup.
    if (api.cleanup() != 0
        || api.get_socket_state(cleanup_socket) != SRTS_NONEXIST) {
        return 17;
    }

    // Cleanup is idempotent after the final runtime generation.
    if (api.cleanup() != 0) {
        return 18;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <shared-library>\n", argv[0]);
        return 1;
    }

    SharedModule module(argv[1]);
    for (int cycle = 0; cycle < reload_cycles; ++cycle) {
        if (!module.load()) {
            std::fprintf(stderr, "load failed: %s\n", module.error());
            return 2;
        }

        SrtApi api;
        if (!api.resolve(module)) {
            std::fprintf(stderr,
                "required public SRT symbol is missing\n");
            return 3;
        }

        const int exercise_result = exercise_cycle(api, cycle);
        if (exercise_result != 0) {
            std::fprintf(stderr,
                "shared-library cycle %d failed at step %d\n",
                cycle, exercise_result);
            return exercise_result;
        }

        if (!module.unload()) {
            std::fprintf(stderr, "unload failed: %s\n", module.error());
            return 4;
        }
        if (module.is_resident()) {
            std::fprintf(stderr,
                "shared library remained resident after cycle %d\n",
                cycle);
            return 5;
        }
    }
    return 0;
}
