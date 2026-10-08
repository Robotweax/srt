#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace robotweax::srt::test {
namespace detail {

inline std::atomic<std::uint64_t> temporary_directory_id {0};

#ifdef _WIN32
[[noreturn]] inline void throw_directory_error(const char* operation)
{
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(), operation);
}
#endif

// Reserve the directory and its access restriction together. Never adopt an
// existing entry, including a symlink or a directory created by another user.
inline bool create_private_directory(const std::filesystem::path& path)
{
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH> volume {};
    DWORD flags = 0;
    if (!GetVolumePathNameW(path.parent_path().c_str(), volume.data(),
            static_cast<DWORD>(volume.size()))
        || !GetVolumeInformationW(
            volume.data(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0))
        throw_directory_error("temporary directory volume");
    if ((flags & FS_PERSISTENT_ACLS) == 0)
        throw std::runtime_error(
            "temporary directory requires filesystem ACLs");

    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
        throw_directory_error("temporary directory process token");
    const std::unique_ptr<void, decltype(&CloseHandle)> token {
        raw_token, &CloseHandle};
    DWORD required = 0;
    if (GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required)
        || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        throw_directory_error("temporary directory token size");
    std::vector<std::byte> user_storage(required);
    if (!GetTokenInformation(
            token.get(), TokenUser, user_storage.data(), required, &required))
        throw_directory_error("temporary directory token user");
    const auto user = reinterpret_cast<TOKEN_USER*>(user_storage.data());
    alignas(ACL) std::array<std::byte,
        sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE>
        acl_storage {};
    const auto acl = reinterpret_cast<ACL*>(acl_storage.data());
    SECURITY_DESCRIPTOR descriptor {};
    if (!InitializeAcl(
            acl, static_cast<DWORD>(acl_storage.size()), ACL_REVISION)
        || !AddAccessAllowedAceEx(acl, ACL_REVISION,
            OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, FILE_ALL_ACCESS,
            user->User.Sid)
        || !InitializeSecurityDescriptor(
            &descriptor, SECURITY_DESCRIPTOR_REVISION)
        || !SetSecurityDescriptorOwner(&descriptor, user->User.Sid, FALSE)
        || !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)
        || !SetSecurityDescriptorControl(
            &descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        throw_directory_error("temporary directory security descriptor");
    SECURITY_ATTRIBUTES attributes {
        sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    if (CreateDirectoryW(path.c_str(), &attributes))
        return true;
    const auto error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
        return false;
    throw_directory_error("create private temporary directory");
#else
    if (::mkdir(path.c_str(), 0700) == 0)
        return true;
    const auto error = errno;
    if (error == EEXIST)
        return false;
    throw std::system_error(
        error, std::generic_category(), "create private temporary directory");
#endif
}

} // namespace detail

class PrivateTemporaryDirectory {
public:
    explicit PrivateTemporaryDirectory(
        const std::filesystem::path& parent =
            std::filesystem::temp_directory_path())
    {
#ifdef _WIN32
        const auto process_id = GetCurrentProcessId();
#else
        const auto process_id = ::getpid();
#endif
        for (std::size_t attempt = 0; attempt < 128; ++attempt) {
            auto candidate = parent
                / ("robotweax-srt-files-" + std::to_string(process_id) + "-"
                    + std::to_string(detail::temporary_directory_id.fetch_add(
                        1, std::memory_order_relaxed)));
            if (detail::create_private_directory(candidate)) {
                directory_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("temporary directory collision limit reached");
    }

    ~PrivateTemporaryDirectory()
    {
        std::error_code ignored;
        (void)std::filesystem::remove_all(directory_, ignored);
    }

    PrivateTemporaryDirectory(const PrivateTemporaryDirectory&) = delete;
    PrivateTemporaryDirectory& operator=(
        const PrivateTemporaryDirectory&) = delete;
    PrivateTemporaryDirectory(PrivateTemporaryDirectory&&) = delete;
    PrivateTemporaryDirectory& operator=(PrivateTemporaryDirectory&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return directory_;
    }

private:
    std::filesystem::path directory_;
};

} // namespace robotweax::srt::test
