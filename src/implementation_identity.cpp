#include "robotweax/srt/implementation_identity.hpp"

#include <algorithm>

namespace robotweax::srt {
namespace {
constexpr std::array<std::byte, 4> magic {
    std::byte {'S'}, std::byte {'R'}, std::byte {'T'}, std::byte {'I'}};

bool alphanumeric(unsigned char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9');
}
bool valid_text(std::string_view text, bool name) noexcept
{
    if (text.empty())
        return !name;
    if (!alphanumeric(static_cast<unsigned char>(text.front())))
        return false;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (!alphanumeric(c) && c != '.' && c != '_' && c != '+' && c != '-'
            && !(name && c == ' '))
            return false;
    }
    return !name || text.back() != ' ';
}
bool valid(const ImplementationIdentity& identity) noexcept
{
    return identity.name_size > 0
        && identity.name_size <= maximum_implementation_name_size
        && identity.release_size <= maximum_implementation_release_size
        && valid_text({identity.name.data(), identity.name_size}, true)
        && valid_text({identity.release.data(), identity.release_size}, false);
}
}
ImplementationIdentity make_implementation_identity(
    std::string_view name, std::string_view release) noexcept
{
    if (name.size() > maximum_implementation_name_size
        || release.size() > maximum_implementation_release_size
        || !valid_text(name, true) || !valid_text(release, false))
        return {};
    ImplementationIdentity identity;
    std::copy(name.begin(), name.end(), identity.name.begin());
    std::copy(release.begin(), release.end(), identity.release.begin());
    identity.name_size = static_cast<std::uint8_t>(name.size());
    identity.release_size = static_cast<std::uint8_t>(release.size());
    return identity;
}
ImplementationIdentity decode_implementation_identity(
    std::span<const std::byte> content) noexcept
{
    if (content.size() < implementation_identity_prefix_size
        || !std::equal(magic.begin(), magic.end(), content.begin())
        || content[4] != std::byte {1} || content[5] != std::byte {0})
        return {};
    for (std::size_t i = 8; i < 12; ++i)
        if (content[i] != std::byte {0})
            return {};
    const auto name_size = std::to_integer<std::size_t>(content[6]);
    const auto release_size = std::to_integer<std::size_t>(content[7]);
    const auto used =
        implementation_identity_prefix_size + name_size + release_size;
    if (name_size > maximum_implementation_name_size
        || release_size > maximum_implementation_release_size
        || content.size() != ((used + 3U) & ~std::size_t {3U}))
        return {};
    for (auto i = used; i < content.size(); ++i)
        if (content[i] != std::byte {0})
            return {};
    const auto* text = reinterpret_cast<const char*>(
        content.data() + implementation_identity_prefix_size);
    return make_implementation_identity(
        {text, name_size}, {text + name_size, release_size});
}
std::size_t implementation_identity_record_size(
    const ImplementationIdentity& identity) noexcept
{
    if (!valid(identity))
        return 0;
    return extension_header_size
        + ((implementation_identity_prefix_size + identity.name_size
               + identity.release_size + 3U)
            & ~std::size_t {3U});
}
ExtensionEncodeResult encode_implementation_identity(
    const ImplementationIdentity& identity,
    std::span<std::byte> destination) noexcept
{
    const auto size = implementation_identity_record_size(identity);
    if (size == 0)
        return {.error = Error::invalid_extension};
    std::array<std::byte, 108> content {};
    std::copy(magic.begin(), magic.end(), content.begin());
    content[4] = std::byte {1};
    content[6] = static_cast<std::byte>(identity.name_size);
    content[7] = static_cast<std::byte>(identity.release_size);
    auto offset = implementation_identity_prefix_size;
    for (std::size_t i = 0; i < identity.name_size; ++i)
        content[offset++] = static_cast<std::byte>(identity.name[i]);
    for (std::size_t i = 0; i < identity.release_size; ++i)
        content[offset++] = static_cast<std::byte>(identity.release[i]);
    return encode_extension(static_cast<HandshakeExtensionType>(
                                implementation_identity_extension_type),
        std::span {content}.first(size - extension_header_size), destination);
}
} // namespace robotweax::srt
