#include "test.hpp"

#include "compat/group_replay_buffer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using robotweax::srt::SequenceNumber;
using robotweax::srt::compat::GroupReplayBuffer;
using robotweax::srt::compat::GroupReplayMessage;

TEST(compat_group_replay_buffer_is_bounded_and_wraps_payload_storage)
{
    GroupReplayBuffer replay(10U, 3U);
    const SequenceNumber initial{SequenceNumber::mask - 1U};
    const std::array<std::byte, 6> first{
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
        std::byte{'d'}, std::byte{'e'}, std::byte{'f'}};
    const std::array<std::byte, 3> second{
        std::byte{'g'}, std::byte{'h'}, std::byte{'i'}};
    const std::array<std::byte, 4> third{
        std::byte{'j'}, std::byte{'k'},
        std::byte{'l'}, std::byte{'m'}};
    const auto append = [&replay](std::span<const std::byte> payload,
                            SequenceNumber sequence,
                            std::uint32_t message_number) {
        return replay.append(payload,
            {
                .first_sequence = sequence,
                .next_sequence = sequence.next(),
                .message_number = message_number,
            });
    };

    REQUIRE_EQ(append(first, initial, 1U),
        GroupReplayBuffer::AppendResult::success);
    REQUIRE_EQ(append(second, initial.next(), 2U),
        GroupReplayBuffer::AppendResult::success);
    replay.acknowledge_before(initial.next());
    REQUIRE_EQ(replay.append(third,
                   {
                       .first_sequence = initial.advanced(2U),
                       .next_sequence = initial.advanced(3U),
                       .message_number = 3U,
                       .source_time_microseconds = 123'456,
                       .ttl_milliseconds = 75,
                       .in_order = false,
                   }),
        GroupReplayBuffer::AppendResult::success);
    REQUIRE_EQ(replay.size(), 2U);
    REQUIRE_EQ(replay.buffered_bytes(), 7U);
    REQUIRE_EQ(*replay.first_sequence(), initial.next());
    REQUIRE_EQ(*replay.next_sequence(), initial.advanced(3U));

    std::array<std::byte, 8> copied{};
    GroupReplayMessage metadata;
    std::size_t copied_size = 0;
    REQUIRE(replay.copy_starting_at(initial.advanced(2U),
        copied, metadata, copied_size));
    REQUIRE_EQ(copied_size, third.size());
    REQUIRE(std::equal(third.begin(), third.end(), copied.begin()));
    REQUIRE_EQ(metadata.message_number, 3U);
    REQUIRE_EQ(metadata.source_time_microseconds, 123'456);
    REQUIRE_EQ(metadata.ttl_milliseconds, 75);
    REQUIRE(!metadata.in_order);

    GroupReplayBuffer::Cursor cursor;
    REQUIRE(replay.begin_at(initial.next(), cursor));
    REQUIRE(replay.copy_next(cursor, copied, metadata, copied_size));
    REQUIRE_EQ(metadata.message_number, 2U);
    REQUIRE(replay.copy_next(cursor, copied, metadata, copied_size));
    REQUIRE_EQ(metadata.message_number, 3U);
    REQUIRE(!replay.copy_next(cursor, copied, metadata, copied_size));

    // A fourth six-byte message exceeds the remaining byte budget and evicts
    // only the oldest required entry, without exceeding the ceiling.
    REQUIRE_EQ(append(first, initial.advanced(3U), 4U),
        GroupReplayBuffer::AppendResult::success);
    REQUIRE_EQ(replay.size(), 2U);
    REQUIRE_EQ(replay.buffered_bytes(), third.size() + first.size());
    REQUIRE_EQ(*replay.first_sequence(), initial.advanced(2U));
    REQUIRE(replay.storage_allocated());
    replay.release_storage();
    REQUIRE(!replay.storage_allocated());
    REQUIRE_EQ(replay.size(), 0U);
}

} // namespace
