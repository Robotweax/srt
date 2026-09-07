#include "test.hpp"

#include "robotweax/srt/sequence.hpp"

#include <cstdint>

using robotweax::srt::PacketTimestamp;
using robotweax::srt::SequenceNumber;

TEST(sequence_wraps_at_31_bits)
{
    const SequenceNumber last{SequenceNumber::mask};
    REQUIRE_EQ(last.next().value(), 0U);
    REQUIRE_EQ(SequenceNumber{0}.distance_from(last), 1);
    REQUIRE_EQ(last.distance_from(SequenceNumber{0}), -1);
}

TEST(sequence_comparison_is_wrap_safe)
{
    const SequenceNumber before_wrap{SequenceNumber::mask - 2U};
    const SequenceNumber after_wrap{1};
    REQUIRE(after_wrap.is_after(before_wrap));
    REQUIRE_EQ(after_wrap.distance_from(before_wrap), 4);
}

TEST(timestamp_distance_is_wrap_safe)
{
    const PacketTimestamp before_wrap{0xffff'fff0U};
    const PacketTimestamp after_wrap{0x0000'0010U};
    REQUIRE_EQ(after_wrap.distance_from(before_wrap), 32);
    REQUIRE_EQ(before_wrap.distance_from(after_wrap), -32);
}
