#include "test.hpp"
#include "null_packet_deletion.hpp"
#include "robotweax/srt/handshake_extensions.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <vector>

using namespace robotweax::srt;

namespace {
std::array<std::byte, 188> null_unit()
{
    std::array<std::byte, 188> unit;
    unit.fill(std::byte {0xff});
    unit[0] = std::byte {0x47};
    unit[1] = std::byte {0x1f};
    unit[3] = std::byte {0x10};
    return unit;
}
void round_trip(std::span<const std::byte> source, std::size_t deleted)
{
    std::vector<std::byte> wire(source.size() + 1);
    std::vector<std::byte> restored(source.size());
    const auto encoded = npd::encode_payload(source, wire);
    REQUIRE(encoded);
    REQUIRE_EQ(encoded.null_units, deleted);
    REQUIRE_EQ(encoded.bytes_written, source.size() + 1 - 188 * deleted);
    const auto decoded = npd::decode_payload(
        std::span {wire}.first(encoded.bytes_written), restored);
    REQUIRE(decoded);
    REQUIRE_EQ(decoded.bytes_written, source.size());
    REQUIRE_EQ(decoded.null_units, deleted);
    REQUIRE(std::equal(source.begin(), source.end(), restored.begin()));
}
} // namespace

TEST(npd_record_has_exact_wire_bytes_and_rejects_every_single_bit_mutation)
{
    constexpr std::array<std::byte, 12> expected {std::byte {0x7f},
        std::byte {0x15}, std::byte {0}, std::byte {2}, std::byte {0x53},
        std::byte {0x4e}, std::byte {0x50}, std::byte {0x44}, std::byte {1},
        std::byte {0}, std::byte {0}, std::byte {0}};
    std::array<std::byte, 12> bytes {};
    REQUIRE_EQ(npd::encode_record(bytes).bytes_written, expected.size());
    REQUIRE_EQ(bytes, expected);
    REQUIRE_EQ(npd::decode_record(bytes), Error::none);
    const auto extension = decode_extension(bytes);
    REQUIRE(extension);
    REQUIRE_EQ(static_cast<std::uint16_t>(extension.extension.type), 0x7f15);
    REQUIRE_EQ(extension.extension.content.size(), 8U);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto mutation = bytes;
            mutation[i] ^= static_cast<std::byte>(1U << bit);
            REQUIRE_EQ(npd::decode_record(mutation), Error::invalid_extension);
        }
        REQUIRE_EQ(npd::decode_record(std::span {bytes}.first(i)),
            Error::invalid_extension);
        auto destination = expected;
        REQUIRE_EQ(npd::encode_record(std::span {destination}.first(i)).error,
            Error::buffer_too_small);
        REQUIRE_EQ(destination, expected);
    }
    std::array<std::byte, 13> trailing {};
    std::copy(bytes.begin(), bytes.end(), trailing.begin());
    REQUIRE_EQ(npd::decode_record(trailing), Error::invalid_extension);
}

TEST(npd_payload_round_trips_every_deletion_and_retention_combination)
{
    std::size_t cases = 0;
    unsigned combinations = 1;
    for (unsigned units = 1; units <= 7; ++units) {
        combinations *= 3;
        for (unsigned combination = 0; combination < combinations;
            ++combination) {
            std::vector<std::byte> source;
            std::vector<std::byte> wire(1);
            unsigned bitmap = 0;
            unsigned choices = combination;
            std::size_t canonical = 0;
            for (unsigned i = 0; i < units; ++i) {
                const auto choice = choices % 3;
                choices /= 3;
                auto unit = null_unit();
                if (choice == 0) {
                    unit[1] = std::byte {0};
                    unit[4] = static_cast<std::byte>(i);
                } else {
                    ++canonical;
                }
                source.insert(source.end(), unit.begin(), unit.end());
                if (choice == 2) {
                    bitmap |= 1U << i;
                } else {
                    wire.insert(wire.end(), unit.begin(), unit.end());
                }
            }
            wire[0] = static_cast<std::byte>(bitmap);
            std::vector<std::byte> restored(source.size());
            const auto decoded = npd::decode_payload(wire, restored);
            REQUIRE(decoded);
            REQUIRE_EQ(decoded.bytes_written, source.size());
            REQUIRE_EQ(decoded.null_units, std::popcount(bitmap));
            REQUIRE_EQ(restored, source);
            round_trip(source, canonical);
            ++cases;
        }
    }
    REQUIRE_EQ(cases, 3279U);
}

TEST(npd_payload_all_null_is_one_byte_and_requires_full_restoration_capacity)
{
    std::vector<std::byte> source;
    const auto unit = null_unit();
    for (unsigned i = 0; i < 7; ++i) {
        source.insert(source.end(), unit.begin(), unit.end());
    }
    std::array<std::byte, 1> wire {};
    REQUIRE_EQ(npd::encode_payload(source, wire).bytes_written, 1U);
    REQUIRE_EQ(wire[0], std::byte {0x7f});
    for (std::size_t size = 0; size < source.size(); ++size) {
        auto destination = source;
        REQUIRE_EQ(
            npd::decode_payload(wire, std::span {destination}.first(size))
                .error,
            Error::buffer_too_small);
        REQUIRE_EQ(destination, source);
    }
}

TEST(npd_payload_preserves_noncanonical_unaligned_and_oversized_ts)
{
    const auto unit = null_unit();
    for (std::size_t i = 0; i < unit.size(); ++i) {
        auto changed = unit;
        changed[i] ^= std::byte {1};
        round_trip(changed, 0);
    }
    std::vector<std::byte> source;
    for (unsigned i = 0; i < 8; ++i) {
        source.insert(source.end(), unit.begin(), unit.end());
    }
    round_trip(source, 0);
    source.resize(7 * 188);
    source[188] =
        std::byte {0}; // Lost sync disables deletion for the whole DATA.
    round_trip(source, 0);
    source.resize(188 + 1);
    round_trip(source, 0);
    for (std::size_t size = 1; size < 188; ++size) {
        round_trip(std::span {unit}.first(size), 0);
    }
}

TEST(npd_payload_validates_all_prefixes_and_lengths_without_partial_writes)
{
    const auto sentinel = std::byte {0xa5};
    std::array<std::byte, 1504> destination;
    destination.fill(sentinel);
    const auto original = destination;
    // Include every byte length around each unit boundary, and all 256 prefixes.
    for (unsigned prefix = 0; prefix < 256; ++prefix) {
        for (std::size_t length = 0; length <= 1504; ++length) {
            std::vector<std::byte> wire(length + 1);
            wire[0] = static_cast<std::byte>(prefix);
            const auto count = length / 188 + std::popcount(prefix);
            const bool valid = prefix < 128
                && (prefix == 0 ? length != 0
                                : length % 188 == 0 && count <= 7
                            && std::bit_width(prefix) <= count);
            const auto decoded = npd::decode_payload(wire, destination);
            REQUIRE_EQ(static_cast<bool>(decoded), valid);
            if (!valid) {
                REQUIRE_EQ(decoded.error, Error::invalid_payload_size);
                REQUIRE_EQ(decoded.bytes_written, 0U);
                REQUIRE_EQ(decoded.null_units, 0U);
                REQUIRE_EQ(destination, original);
            }
            destination = original;
        }
    }
    REQUIRE_EQ(npd::decode_payload({}, destination).error,
        Error::invalid_payload_size);
    REQUIRE_EQ(npd::encode_payload({}, destination).error,
        Error::invalid_payload_size);
    REQUIRE_EQ(destination, original);
    const auto source = null_unit();
    for (std::size_t size = 0; size < 188; ++size) {
        REQUIRE_EQ(npd::encode_payload(std::span {source}.first(187),
                       std::span {destination}.first(size))
                       .error,
            Error::buffer_too_small);
        REQUIRE_EQ(destination, original);
    }
    REQUIRE_EQ(npd::encode_payload(source, {}).error, Error::buffer_too_small);
}
