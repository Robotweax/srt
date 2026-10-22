#include "member_filter.hpp"
#include "robotweax/srt/socket_options.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

using namespace robotweax::srt;
using namespace robotweax::srt::extensions;

namespace {
std::uint8_t byte(
    std::span<const std::uint8_t> bytes, std::size_t index) noexcept
{
    return index < bytes.size() ? bytes[index] : 0;
}
void check(const MemberFilterReceiveResult& result, SequenceNumber first,
    std::size_t capacity, std::size_t maximum_payload) noexcept
{
    if (result.error != Error::none
        || result.recovered.size() > MemberFilterRuntime::maximum_outputs)
        std::abort();
    for (const auto& packet : result.recovered) {
        const auto offset = packet.data.sequence.distance_from(first);
        if (!result.admitted || packet.kind != PacketKind::data
            || !packet.data.message_number
            || packet.data.encryption_key != EncryptionKey::none
            || packet.data.destination_socket_id != 22 || packet.payload.empty()
            || packet.payload.size() > maximum_payload || offset < 0
            || static_cast<std::size_t>(offset) >= capacity)
            std::abort();
    }
}
}

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data, std::size_t size)
{
    const auto bytes = std::span {data, size};
    const auto input = std::as_bytes(bytes);
    if (size <= maximum_packet_filter_configuration_size) {
        SocketOptions options;
        (void)options.set_packet_filter(
            {reinterpret_cast<const char*>(data), size});
    }
    const auto* filter = compiled_filter("rwx-repeat-v1");
    if (!filter)
        return 0; // Other trusted provider builds need their own symbol oracle.
    const std::size_t capacity = 1U + byte(bytes, 0) % 32U;
    const std::size_t maximum_payload =
        1U + ((std::size_t {byte(bytes, 1)} << 8U) | byte(bytes, 2)) % 1433U;
    SequenceNumber first {
        byte(bytes, 3) & 1U ? SequenceNumber::mask - 2U : 100U};
    MemberFilterRuntime sender;
    MemberFilterRuntime receiver;
    if (sender.start(
            *filter, 11, 22, first, first, capacity, capacity, maximum_payload)
            != Error::none
        || receiver.start(*filter, 22, 11, first, first, capacity, capacity,
               maximum_payload)
            != Error::none)
        std::abort();
    std::array<std::byte, 1433> payload {};
    const auto payload_size =
        std::min(maximum_payload, std::max<std::size_t>(1, size));
    std::copy_n(
        input.begin(), std::min(input.size(), payload_size), payload.begin());
    for (std::size_t step = 0; step < 8; ++step) {
        const std::uint64_t now = step * 10;
        const PacketView source {.kind = PacketKind::data,
            .data = {.sequence = first,
                .message_number = 1,
                .boundary = MessageBoundary::solo,
                .timestamp = PacketTimestamp {123},
                .destination_socket_id = 22},
            .payload = std::span {payload}.first(payload_size)};
        if (sender.source_sent(source, first, now) != Error::none
            || !sender.pending())
            std::abort();
        const auto repair = *sender.pending();
        std::array<std::byte, 1500> changed {};
        std::copy(repair.begin(), repair.end(), changed.begin());
        const auto selector = byte(bytes, step * 4 + 4);
        const auto position = ((std::size_t {byte(bytes, step * 4 + 5)} << 8U)
                                  | byte(bytes, step * 4 + 6))
            % repair.size();
        changed[position] ^= static_cast<std::byte>(byte(bytes, step * 4 + 7));
        const auto decoded =
            decode_packet(std::span {changed}.first(repair.size()));
        if (decoded)
            check(receiver.receive_repair(decoded.packet, first, now + 1),
                first, capacity, maximum_payload);
        // Exercise arbitrary/truncated bodies with an otherwise valid outer route.
        const auto valid = decode_packet(repair);
        if (!valid)
            std::abort();
        auto opaque = valid.packet;
        opaque.payload = input.first(std::min<std::size_t>(size, 1500));
        opaque.control.type_specific = first.advanced(selector).value();
        check(receiver.receive_repair(opaque, first, now + 2), first, capacity,
            maximum_payload);
        const auto raw =
            decode_packet(input.first(std::min<std::size_t>(size, 1500)));
        if (raw) {
            check(receiver.receive_source(raw.packet, first, now + 3), first,
                capacity, maximum_payload);
            check(receiver.receive_repair(raw.packet, first, now + 4), first,
                capacity, maximum_payload);
        }
        // A malformed packet must not poison this example decoder. Check the
        // known-good packet and its replay after each mutation, including wrap.
        for (unsigned replay = 0; replay < 2; ++replay) {
            const auto recovered =
                receiver.receive_repair(valid.packet, first, now + 5 + replay);
            check(recovered, first, capacity, maximum_payload);
            if (recovered.recovered.size() != 1
                || !std::equal(source.payload.begin(), source.payload.end(),
                    recovered.recovered[0].payload.begin(),
                    recovered.recovered[0].payload.end())
                || recovered.recovered[0].data.sequence != first)
                std::abort();
        }
        sender.consume();
        first = first.next();
    }
    return 0;
}
