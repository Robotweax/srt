#include "null_packet_deletion.hpp"

#include <algorithm>
#include <bit>

namespace robotweax::srt::npd {
namespace {
constexpr auto canonical_null = [] {
    std::array<std::byte, unit_size> unit {};
    unit.fill(std::byte {0xff});
    unit[0] = std::byte {0x47};
    unit[1] = std::byte {0x1f};
    unit[3] = std::byte {0x10};
    return unit;
}();

constexpr std::array<std::byte, 4> record_header {
    static_cast<std::byte>(extension_command >> 8U),
    static_cast<std::byte>(extension_command & 0xffU), std::byte {0},
    std::byte {2}};
} // namespace

Result encode_record(std::span<std::byte> destination) noexcept
{
    const auto size = record_header.size() + record_content.size();
    if (destination.size() < size) {
        return {.error = Error::buffer_too_small};
    }
    std::copy(record_header.begin(), record_header.end(), destination.begin());
    std::copy(record_content.begin(), record_content.end(),
        destination.begin() + record_header.size());
    return {.bytes_written = size};
}

Error decode_record(std::span<const std::byte> record) noexcept
{
    if (record.size() != record_header.size() + record_content.size()
        || !std::equal(
            record_header.begin(), record_header.end(), record.begin())
        || !std::equal(record_content.begin(), record_content.end(),
            record.begin() + record_header.size())) {
        return Error::invalid_extension;
    }
    return Error::none;
}

Result encode_payload(std::span<const std::byte> application,
    std::span<std::byte> destination) noexcept
{
    if (application.empty()) {
        return {.error = Error::invalid_payload_size};
    }
    unsigned bitmap = 0;
    const auto units = application.size() / unit_size;
    if (application.size() % unit_size == 0 && units <= maximum_units) {
        bool aligned = true;
        for (std::size_t i = 0; i < units; ++i) {
            const auto unit = application.subspan(i * unit_size, unit_size);
            if (unit[0] != std::byte {0x47}) {
                aligned = false;
                break;
            }
            if (std::equal(unit.begin(), unit.end(), canonical_null.begin())) {
                bitmap |= 1U << i;
            }
        }
        if (!aligned) {
            bitmap = 0;
        }
    }
    const auto deleted = static_cast<std::size_t>(std::popcount(bitmap));
    const auto remaining = application.size() - deleted * unit_size;
    // Compare before adding the prefix to avoid size overflow.
    if (destination.size() <= remaining) {
        return {.error = Error::buffer_too_small};
    }
    destination[0] = static_cast<std::byte>(bitmap);
    if (bitmap == 0) {
        std::copy(
            application.begin(), application.end(), destination.begin() + 1);
    } else {
        std::size_t offset = 1;
        for (std::size_t i = 0; i < units; ++i) {
            if ((bitmap & (1U << i)) == 0) {
                const auto unit = application.subspan(i * unit_size, unit_size);
                std::copy(unit.begin(), unit.end(),
                    destination.subspan(offset).begin());
                offset += unit_size;
            }
        }
    }
    return {.bytes_written = remaining + 1, .null_units = deleted};
}

Result decode_payload(
    std::span<const std::byte> wire, std::span<std::byte> destination) noexcept
{
    if (wire.empty()) {
        return {.error = Error::invalid_payload_size};
    }
    const auto bitmap = std::to_integer<unsigned>(wire[0]);
    const auto rest = wire.subspan(1);
    if ((bitmap & 0x80U) != 0 || (bitmap == 0 && rest.empty())) {
        return {.error = Error::invalid_payload_size};
    }
    const auto restored = static_cast<std::size_t>(std::popcount(bitmap));
    if (bitmap != 0) {
        // Bound the received count before adding the deleted-unit count.
        if (rest.size() % unit_size != 0
            || rest.size() / unit_size > maximum_units - restored) {
            return {.error = Error::invalid_payload_size};
        }
        const auto units = rest.size() / unit_size + restored;
        if ((bitmap >> units) != 0) {
            return {.error = Error::invalid_payload_size};
        }
    }
    const auto size = rest.size() + restored * unit_size;
    if (destination.size() < size) {
        return {.error = Error::buffer_too_small};
    }
    if (bitmap == 0) {
        std::copy(rest.begin(), rest.end(), destination.begin());
    } else {
        std::size_t offset = 0;
        for (std::size_t i = 0; i < size / unit_size; ++i) {
            const bool deleted = (bitmap & (1U << i)) != 0;
            const auto unit = deleted
                ? std::span<const std::byte> {canonical_null}
                : rest.subspan(offset, unit_size);
            std::copy(unit.begin(), unit.end(),
                destination.subspan(i * unit_size).begin());
            if (!deleted) {
                offset += unit_size;
            }
        }
    }
    return {.bytes_written = size, .null_units = restored};
}
} // namespace robotweax::srt::npd
