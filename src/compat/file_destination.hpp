#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

namespace robotweax::srt::compat {

// One file identity from non-destructive open through delayed replacement.
// Parent-directory authorization belongs to the caller. Final symlinks/reparse
// points and non-regular files are rejected.
class FileDestination {
public:
    FileDestination() = default;
    ~FileDestination();
    FileDestination(const FileDestination&) = delete;
    FileDestination& operator=(const FileDestination&) = delete;
    [[nodiscard]] bool open(const char* path) noexcept;
    [[nodiscard]] bool seek(std::int64_t offset) noexcept;
    [[nodiscard]] bool truncate() noexcept;
    [[nodiscard]] bool write(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] bool flush() noexcept;

private:
    std::FILE* file_ = nullptr;
};

} // namespace robotweax::srt::compat
