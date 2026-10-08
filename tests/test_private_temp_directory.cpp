#include "test.hpp"
#include "private_temp_directory.hpp"

#include <array>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#endif

using namespace robotweax::srt::test;

namespace {

void require_owner_only_directory(const std::filesystem::path& path)
{
#ifdef _WIN32
    auto name = path.native();
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    REQUIRE_EQ(GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                   OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                   &owner, nullptr, &acl, nullptr, &raw_descriptor),
        ERROR_SUCCESS);
    const std::unique_ptr<void, decltype(&LocalFree)> descriptor {
        raw_descriptor, &LocalFree};
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    REQUIRE(
        GetSecurityDescriptorControl(descriptor.get(), &control, &revision));
    REQUIRE((control & SE_DACL_PROTECTED) != 0);
    REQUIRE(acl != nullptr);
    REQUIRE_EQ(acl->AceCount, 1U);
    void* raw_ace = nullptr;
    REQUIRE(GetAce(acl, 0, &raw_ace));
    const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
    REQUIRE_EQ(ace->Header.AceType, ACCESS_ALLOWED_ACE_TYPE);
    REQUIRE_EQ(ace->Mask, FILE_ALL_ACCESS);
    REQUIRE((ace->Header.AceFlags & OBJECT_INHERIT_ACE) != 0);
    REQUIRE((ace->Header.AceFlags & CONTAINER_INHERIT_ACE) != 0);
    REQUIRE(EqualSid(owner, &ace->SidStart));
    HANDLE raw_token = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token));
    const std::unique_ptr<void, decltype(&CloseHandle)> token {
        raw_token, &CloseHandle};
    DWORD required = 0;
    REQUIRE(
        !GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required));
    REQUIRE_EQ(GetLastError(), ERROR_INSUFFICIENT_BUFFER);
    std::vector<std::byte> user_storage(required);
    REQUIRE(GetTokenInformation(
        token.get(), TokenUser, user_storage.data(), required, &required));
    const auto user = reinterpret_cast<TOKEN_USER*>(user_storage.data());
    REQUIRE(EqualSid(owner, user->User.Sid));
#else
    struct stat status {};
    REQUIRE_EQ(::lstat(path.c_str(), &status), 0);
    REQUIRE(S_ISDIR(status.st_mode));
    REQUIRE_EQ(status.st_uid, ::geteuid());
    REQUIRE_EQ(status.st_mode & 0777, 0700);
#endif
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char> {input},
        std::istreambuf_iterator<char> {}};
}

std::filesystem::path candidate_path(
    const std::filesystem::path& parent, std::uint64_t identifier)
{
#ifdef _WIN32
    const auto process_id = GetCurrentProcessId();
#else
    const auto process_id = ::getpid();
#endif
    return parent
        / ("robotweax-srt-files-" + std::to_string(process_id) + "-"
            + std::to_string(identifier));
}

} // namespace

TEST(private_temp_directory_restricts_access_at_creation_and_cleans_on_unwind)
{
    std::filesystem::path created;
    try {
        PrivateTemporaryDirectory directory;
        created = directory.path();
        require_owner_only_directory(created);
        REQUIRE(!std::filesystem::exists(created / "source"));
        REQUIRE(!std::filesystem::exists(created / "destination"));
        std::ofstream file(created / "source");
        file << "fixture data";
        REQUIRE(file.good());
        throw std::logic_error("fixture aborted after file creation");
    } catch (const std::logic_error& error) {
        REQUIRE_EQ(
            std::string {error.what()}, "fixture aborted after file creation");
    }
    REQUIRE(!created.empty());
    REQUIRE(!std::filesystem::exists(created));
}

TEST(private_temp_directory_rejects_occupied_paths_without_adopting_them)
{
    PrivateTemporaryDirectory parent;
    const auto file = parent.path() / "occupied-file";
    const auto directory = parent.path() / "occupied-directory";
    {
        std::ofstream output(file);
        output << "untouched";
        REQUIRE(output.good());
    }
    REQUIRE(std::filesystem::create_directory(directory));
    REQUIRE(!detail::create_private_directory(file));
    REQUIRE(!detail::create_private_directory(directory));
    REQUIRE_EQ(read_file(file), "untouched");
    REQUIRE(std::filesystem::is_directory(directory));
}

TEST(private_temp_directory_retries_collisions_with_a_new_private_reservation)
{
    PrivateTemporaryDirectory parent;
    const auto identifier = detail::temporary_directory_id.load();
    const auto file = candidate_path(parent.path(), identifier);
    const auto occupied = candidate_path(parent.path(), identifier + 1U);
    {
        std::ofstream output(file);
        output << "untouched";
        REQUIRE(output.good());
    }
    REQUIRE(std::filesystem::create_directory(occupied));
    std::filesystem::path created;
    {
        PrivateTemporaryDirectory directory(parent.path());
        created = directory.path();
        REQUIRE(created != file);
        REQUIRE(created != occupied);
        require_owner_only_directory(created);
    }
    REQUIRE(!std::filesystem::exists(created));
    REQUIRE_EQ(read_file(file), "untouched");
    REQUIRE(std::filesystem::is_directory(occupied));
}

TEST(private_temp_directory_parallel_fixtures_keep_distinct_live_reservations)
{
    PrivateTemporaryDirectory parent;
    std::array<std::unique_ptr<PrivateTemporaryDirectory>, 32> directories {};
    std::array<std::exception_ptr, 8> errors {};
    std::vector<std::jthread> workers;
    for (std::size_t worker = 0; worker < errors.size(); ++worker) {
        workers.emplace_back([&, worker] {
            try {
                for (std::size_t item = 0; item < 4; ++item)
                    directories[worker * 4 + item] =
                        std::make_unique<PrivateTemporaryDirectory>(
                            parent.path());
            } catch (...) {
                errors[worker] = std::current_exception();
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    for (const auto& error : errors)
        if (error)
            std::rethrow_exception(error);
    std::set<std::filesystem::path> paths;
    for (const auto& directory : directories) {
        REQUIRE(directory != nullptr);
        require_owner_only_directory(directory->path());
        REQUIRE(paths.insert(directory->path()).second);
    }
    REQUIRE_EQ(paths.size(), directories.size());
    for (auto& directory : directories)
        directory.reset();
    for (const auto& path : paths)
        REQUIRE(!std::filesystem::exists(path));
}

TEST(private_temp_directory_collision_exhaustion_preserves_existing_entries)
{
    PrivateTemporaryDirectory parent;
    const auto identifier = detail::temporary_directory_id.load();
    for (std::size_t item = 0; item < 128; ++item) {
        std::ofstream output(candidate_path(parent.path(), identifier + item));
        output << "occupied";
        REQUIRE(output.good());
    }
    bool refused = false;
    try {
        PrivateTemporaryDirectory directory(parent.path());
    } catch (const std::runtime_error& error) {
        REQUIRE_EQ(std::string {error.what()},
            "temporary directory collision limit reached");
        refused = true;
    }
    REQUIRE(refused);
    for (std::size_t item = 0; item < 128; ++item)
        REQUIRE_EQ(read_file(candidate_path(parent.path(), identifier + item)),
            "occupied");
}

TEST(private_temp_directory_rejects_symlinks_and_cleanup_does_not_follow_them)
{
    PrivateTemporaryDirectory parent;
    const auto outside = parent.path() / "outside";
    REQUIRE(std::filesystem::create_directory(outside));
    const auto victim = outside / "untouched";
    {
        std::ofstream output(victim);
        output << "outside data";
        REQUIRE(output.good());
    }
    const auto planted = parent.path() / "planted-link";
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, planted, error);
#ifdef _WIN32
    SKIP_UNLESS(error.value() != ERROR_PRIVILEGE_NOT_HELD,
        "Windows symlink creation requires runner privilege");
#endif
    REQUIRE(!error);
    REQUIRE(!detail::create_private_directory(planted));
    REQUIRE(std::filesystem::is_symlink(planted));
    std::filesystem::path created;
    {
        PrivateTemporaryDirectory directory(parent.path());
        created = directory.path();
        std::filesystem::create_directory_symlink(outside, created / "link");
    }
    REQUIRE(!std::filesystem::exists(created));
    REQUIRE_EQ(read_file(victim), "outside data");
    REQUIRE(std::filesystem::is_symlink(planted));
}
