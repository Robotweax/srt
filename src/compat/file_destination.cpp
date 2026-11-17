#include "compat/file_destination.hpp"

#include <limits>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace robotweax::srt::compat {

FileDestination::~FileDestination()
{
    if (file_ != nullptr)
        std::fclose(file_);
}

bool FileDestination::open(const char* path) noexcept
{
    if (file_ != nullptr || path == nullptr)
        return false;
#ifdef _WIN32
    const HANDLE handle = CreateFileA(path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION information {};
    if (!GetFileInformationByHandle(handle, &information)
        || GetFileType(handle) != FILE_TYPE_DISK
        || (information.dwFileAttributes
               & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
            != 0) {
        CloseHandle(handle);
        return false;
    }
    const int descriptor = _open_osfhandle(
        reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_BINARY);
    if (descriptor == -1) {
        CloseHandle(handle);
        return false;
    }
    file_ = _fdopen(descriptor, "wb");
    if (file_ == nullptr)
        _close(descriptor);
#else
    // O_NONBLOCK also keeps opening a FIFO from blocking before fstat rejects
    // it. It has no effect on regular file I/O.
    const int descriptor = ::open(
        path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0666);
    if (descriptor == -1)
        return false;
    struct stat information {};
    if (::fstat(descriptor, &information) != 0
        || (!S_ISREG(information.st_mode) && !S_ISCHR(information.st_mode))) {
        ::close(descriptor);
        return false;
    }
    regular_file_ = S_ISREG(information.st_mode);
    // fdopen does not truncate the descriptor, unlike fopen("wb").
    file_ = ::fdopen(descriptor, "wb");
    if (file_ == nullptr)
        ::close(descriptor);
#endif
    return file_ != nullptr;
}

bool FileDestination::seek(std::int64_t offset) noexcept
{
    if (file_ == nullptr || offset < 0)
        return false;
#ifdef _WIN32
    return _fseeki64(file_, offset, SEEK_SET) == 0;
#else
    if (static_cast<std::uint64_t>(offset)
        > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        return false;
    return ::fseeko(file_, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool FileDestination::truncate() noexcept
{
    if (file_ == nullptr || std::fflush(file_) != 0)
        return false;
#ifdef _WIN32
    if (_chsize_s(_fileno(file_), 0) != 0)
        return false;
#else
    // Opening character devices with O_TRUNC historically left them intact.
    // Preserve that behavior, including deferred /dev/full write errors.
    if (!regular_file_)
        return true;
    if (::ftruncate(::fileno(file_), 0) != 0)
        return false;
#endif
    return seek(0);
}

bool FileDestination::write(std::span<const std::byte> bytes) noexcept
{
    return file_ != nullptr
        && std::fwrite(bytes.data(), 1, bytes.size(), file_) == bytes.size();
}

bool FileDestination::flush() noexcept
{
    return file_ != nullptr && std::fflush(file_) == 0;
}

} // namespace robotweax::srt::compat
