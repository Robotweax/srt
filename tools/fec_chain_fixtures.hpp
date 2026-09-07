#pragma once

#include "robotweax/srt/packet_filter.hpp"
#include "robotweax/srt/sequence.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace robotweax::srt::fec_chain_fixture {

struct Fixture {
    std::string_view name;
    std::uint32_t columns;
    std::uint32_t rows;
    PacketFilterLayout layout;
    SequenceNumber initial_sequence;
    std::size_t source_count;
    std::size_t receive_capacity;
    std::size_t maximum_payload_size;
    std::size_t expected_control_count;
    // cycle[0] is the final physical DATA trigger. The remaining entries are
    // the exact reconstruction order expected from the row-first Matrix
    // decoder. Source indices are relative to initial_sequence.
    std::span<const std::uint32_t> cycle;
};

// One complete 32x32 product-code cycle. Delivering source zero last must
// peel the other 63 sources by alternating row and column reconstruction.
inline constexpr std::array even_32x32_cycle {
    0U,
    31U,
    1'023U,
    1'022U,
    990U,
    989U,
    957U,
    956U,
    924U,
    923U,
    891U,
    890U,
    858U,
    857U,
    825U,
    824U,
    792U,
    791U,
    759U,
    758U,
    726U,
    725U,
    693U,
    692U,
    660U,
    659U,
    627U,
    626U,
    594U,
    593U,
    561U,
    560U,
    528U,
    527U,
    495U,
    494U,
    462U,
    461U,
    429U,
    428U,
    396U,
    395U,
    363U,
    362U,
    330U,
    329U,
    297U,
    296U,
    264U,
    263U,
    231U,
    230U,
    198U,
    197U,
    165U,
    164U,
    132U,
    131U,
    99U,
    98U,
    66U,
    65U,
    33U,
    32U,
};

// A retained staircase-placement cycle. Columns 0..15 and 32 all intersect
// rows 15..31 in the first 64x32 staircase series, forming a 34-edge cycle.
inline constexpr std::array staircase_64x32_cycle {
    960U,
    992U,
    2'016U,
    1'999U,
    1'935U,
    1'934U,
    1'870U,
    1'869U,
    1'805U,
    1'804U,
    1'740U,
    1'739U,
    1'675U,
    1'674U,
    1'610U,
    1'609U,
    1'545U,
    1'544U,
    1'480U,
    1'479U,
    1'415U,
    1'414U,
    1'350U,
    1'349U,
    1'285U,
    1'284U,
    1'220U,
    1'219U,
    1'155U,
    1'154U,
    1'090U,
    1'089U,
    1'025U,
    1'024U,
};

inline constexpr Fixture even_32x32 {
    .name = "matrix-32x32-even-rollover",
    .columns = 32U,
    .rows = 32U,
    .layout = PacketFilterLayout::even,
    .initial_sequence = SequenceNumber {SequenceNumber::mask - 511U},
    .source_count = 1'024U,
    .receive_capacity = 2'048U,
    .maximum_payload_size = 257U,
    .expected_control_count = 64U,
    .cycle = even_32x32_cycle,
};

inline constexpr Fixture staircase_64x32 {
    .name = "matrix-64x32-staircase-rollover",
    .columns = 64U,
    .rows = 32U,
    .layout = PacketFilterLayout::staircase,
    .initial_sequence = SequenceNumber {SequenceNumber::mask - 2'015U},
    .source_count = 3'008U,
    .receive_capacity = 8'192U,
    .maximum_payload_size = 257U,
    .expected_control_count = 79U,
    .cycle = staircase_64x32_cycle,
};

inline constexpr std::array fixtures {
    even_32x32,
    staircase_64x32,
};

} // namespace robotweax::srt::fec_chain_fixture
