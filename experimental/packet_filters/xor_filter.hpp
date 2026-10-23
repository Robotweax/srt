#pragma once

#include "compiled_filter.hpp"
#include "robotweax/srt/codec.hpp"

#include <algorithm>
#include <bit>
#include <memory>
#include <limits>

namespace robotweax::srt::extensions {
namespace xor8 {
inline constexpr std::size_t symbol_bytes =
    packet_header_size + maximum_data_payload_size;
inline constexpr std::size_t cache_blocks = 64;
struct Block {
    std::array<std::byte, symbol_bytes> parity {};
    std::uint64_t first = 0;
    std::uint32_t wire_first = 0;
    std::uint16_t length_xor = 0;
    std::uint16_t extent = 0;
    std::uint8_t sources = 0;
    bool valid = false;
    bool repair = false;
    bool complete = false;
};
struct State {
    std::array<Block, cache_blocks> blocks {};
};
static_assert(sizeof(State) <= 128 * 1024);

inline void set_sequence(
    std::span<std::byte> bytes, std::uint32_t sequence) noexcept
{
    for (std::size_t i = 0; i < 4; ++i)
        bytes[i] = static_cast<std::byte>((sequence >> ((3 - i) * 8)) & 255U);
}
inline Error event(std::span<std::byte> storage, const FilterEvent& event,
    FilterOutputWriter& writer)
{
    auto& state = *std::launder(reinterpret_cast<State*>(storage.data()));
    if (event.kind == FilterEventKind::environment_changed) {
        for (auto& block : state.blocks)
            if (block.valid
                && block.first + 7 < event.environment.first_sequence)
                block.valid = false;
    }
    if (!event.input)
        return Error::none;
    const auto& input = *event.input;
    const bool sender = event.environment.context.direction == 0;
    if (input.kind == FilterInputKind::feedback
        || (sender && input.kind != FilterInputKind::source)
        || input.member_sequence > SequenceNumber::mask)
        return Error::none;
    const auto index = static_cast<unsigned>(input.member_sequence & 7U);
    if (input.sequence < index)
        return Error::none;
    const auto first = input.sequence - index;
    if (first > (std::numeric_limits<std::uint64_t>::max)() - 7)
        return Error::none;
    const auto wire_first =
        static_cast<std::uint32_t>(input.member_sequence) & ~std::uint32_t {7};
    auto bytes = input.payload;
    std::array<std::byte, symbol_bytes> normalized;
    if (input.kind == FilterInputKind::source) {
        const auto decoded = decode_packet(bytes);
        if (!decoded || decoded.packet.kind != PacketKind::data
            || decoded.packet.data.sequence.value() != input.member_sequence
            || decoded.packet.data.message_number == 0
            || decoded.packet.data.encryption_key != EncryptionKey::none
            || decoded.packet.payload.empty() || bytes.size() > symbol_bytes
            || (!sender
                && decoded.packet.data.destination_socket_id
                    != event.environment.context.session.value)
            || (sender && decoded.packet.data.retransmitted))
            return Error::none;
        if (decoded.packet.data.retransmitted) {
            auto header = decoded.packet.data;
            header.retransmitted = false;
            const auto encoded =
                encode_packet({.kind = PacketKind::data,
                                  .data = header,
                                  .payload = decoded.packet.payload},
                    normalized);
            if (!encoded)
                return Error::none;
            bytes = std::span {normalized}.first(encoded.bytes_written);
        }
    } else if (index != 7 || bytes.size() <= packet_header_size
        || bytes.size() > symbol_bytes || bytes[0] != std::byte {0x58}
        || bytes[1] != std::byte {1}) {
        return Error::none;
    }
    auto& block = state.blocks[(first / 8) % cache_blocks];
    if (block.valid && block.first > first)
        return Error::
            none; // Late traffic cannot evict a newer colliding block.
    if (!block.valid || block.first != first) {
        block = {};
        block.valid = true;
        block.first = first;
        block.wire_first = wire_first;
    }
    if (block.complete || block.wire_first != wire_first)
        return Error::none;
    if (input.kind == FilterInputKind::source) {
        const auto bit = static_cast<std::uint8_t>(1U << index);
        if ((block.sources & bit) != 0)
            return Error::none;
        block.sources |= bit;
        block.length_xor ^= static_cast<std::uint16_t>(bytes.size());
    } else {
        if (block.repair)
            return Error::none;
        block.repair = true;
        block.length_xor ^= static_cast<std::uint16_t>(
            (std::to_integer<unsigned>(bytes[2]) << 8U)
            | std::to_integer<unsigned>(bytes[3]));
    }
    block.extent =
        std::max(block.extent, static_cast<std::uint16_t>(bytes.size()));
    // The first word is sequence (reconstructable) in DATA and magic/version/
    // length parity in repair. The remaining encoded header and payload XOR
    // at identical offsets, leaving room for metadata at maximum MSS.
    for (std::size_t i = 4; i < bytes.size(); ++i)
        block.parity[i] ^= bytes[i];
    if (sender) {
        if (block.sources != 255)
            return Error::none;
        block.complete = true;
        block.parity[0] = std::byte {0x58};
        block.parity[1] = std::byte {1};
        block.parity[2] = static_cast<std::byte>(block.length_xor >> 8U);
        block.parity[3] = static_cast<std::byte>(block.length_xor & 255U);
        return writer.append({.context = event.environment.context,
            .kind = FilterOutputKind::repair,
            .domain = input.domain,
            .sequence = first + 7,
            .payload = std::span {block.parity}.first(block.extent)});
    }
    if (block.sources == 255) {
        block.complete = true;
        return Error::none;
    }
    if (!block.repair || std::popcount(block.sources) != 7)
        return Error::none;
    block.complete = true;
    const auto missing = static_cast<unsigned>(std::countr_one(block.sources));
    const auto sequence = first + missing;
    const auto length = block.length_xor;
    if (sequence < event.environment.first_sequence
        || sequence >= event.environment.end_sequence
        || length <= packet_header_size || length > block.extent)
        return Error::none;
    for (std::size_t i = length; i < block.extent; ++i)
        if (block.parity[i] != std::byte {})
            return Error::none;
    set_sequence(block.parity, wire_first + missing);
    const auto recovered = std::span {block.parity}.first(length);
    const auto decoded = decode_packet(recovered);
    if (!decoded || decoded.packet.kind != PacketKind::data
        || decoded.packet.data.message_number == 0
        || decoded.packet.data.retransmitted
        || decoded.packet.data.encryption_key != EncryptionKey::none
        || decoded.packet.data.destination_socket_id
            != event.environment.context.session.value
        || decoded.packet.payload.empty())
        return Error::none;
    return writer.append({.context = event.environment.context,
        .kind = FilterOutputKind::recovered_data,
        .domain = input.domain,
        .sequence = sequence,
        .payload = recovered});
}
} // namespace xor8

inline CompiledFilter xor8_filter() noexcept
{
    return {.name = "rwx-xor8-v1",
        .implementation = {
            .descriptor = {.scope = FilterScope::member_transport,
                .feedback = FeedbackRequirement::none,
                .state_bytes = sizeof(xor8::State),
                .maximum_outputs_per_batch = 1,
                .maximum_output_bytes_per_batch = xor8::symbol_bytes},
            .state_alignment = alignof(xor8::State),
            .construct =
                [](std::span<std::byte> storage, const FilterOwner&,
                    FilterLimits, const FilterEnvironment& environment) {
                    if (environment.context.direction > 1)
                        return Error::unsupported;
                    std::construct_at(
                        reinterpret_cast<xor8::State*>(storage.data()));
                    return Error::none;
                },
            .on_event = xor8::event,
            .destroy =
                [](std::span<std::byte> storage) noexcept {
                    std::destroy_at(std::launder(
                        reinterpret_cast<xor8::State*>(storage.data())));
                }}};
}
} // namespace robotweax::srt::extensions
