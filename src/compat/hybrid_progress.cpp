// SPDX-License-Identifier: MIT
#include "compat/hybrid_progress.hpp"
#include <algorithm>

namespace robotweax::srt::compat::hybrid {
static_assert(sizeof(ReceiveProgress) < 64 * 1024);
bool DataHighWater::note(std::uint64_t p, std::uint64_t at) noexcept
{
    if (p == UINT64_MAX)
        return false;
    if (packets != UINT64_MAX)
        ++packets;
    if (!observed || p + 1 > next) {
        observed = true;
        next = p + 1;
        last_advance_us = at;
    }
    return true;
}
void ReceiveProgress::sync_floor(SequenceNumber s) noexcept
{
    if (!state_.valid)
        return;
    const auto delta = s.distance_from(sequence_);
    if (delta < 0 || static_cast<std::uint64_t>(delta) > UINT64_MAX - floor_) {
        state_.valid = false;
        return;
    }
    floor_ += static_cast<std::uint64_t>(delta);
    sequence_ = s;
}
std::optional<std::uint64_t> ReceiveProgress::position(
    SequenceNumber s) const noexcept
{
    const auto delta = s.distance_from(sequence_);
    if (delta == -static_cast<std::int32_t>(SequenceNumber::half_range))
        return {};
    if (delta < 0) {
        const auto back =
            static_cast<std::uint64_t>(-static_cast<std::int64_t>(delta));
        if (back > floor_)
            return {};
        return floor_ - back;
    }
    if (static_cast<std::uint64_t>(delta) > UINT64_MAX - floor_)
        return {};
    return floor_ + static_cast<std::uint64_t>(delta);
}
void ReceiveProgress::receive(
    const PacketView& p, bool unique, DataPath path, std::uint64_t at) noexcept
{
    if (!state_.valid
        || (path != DataPath::unicast && path != DataPath::multicast)
        || p.kind != PacketKind::data || !p.data.message_number
        || p.data.encryption_key != EncryptionKey::none
        || p.payload.size() > maximum_data_payload_size)
        return;
    const auto pos = position(p.data.sequence);
    if (!pos || *pos == UINT64_MAX)
        return;
    std::array<std::byte, packet_header_size + maximum_data_payload_size>
        bytes {};
    auto header = p.data;
    header.retransmitted = false;
    const auto encoded = encode_packet(
        {.kind = PacketKind::data, .data = header, .payload = p.payload},
        bytes);
    if (!encoded)
        return;
    auto& entry = entries_[*pos % capacity];
    if (unique && state_.all.observed && state_.all.next > *pos
        && state_.all.next - *pos > capacity) {
        if (state_.unique_packets != UINT64_MAX)
            ++state_.unique_packets;
        (void)state_.all.note(*pos, at);
        (void)(path == DataPath::multicast ? state_.multicast : state_.unicast)
            .note(*pos, at);
        return;
    }
    if (unique) {
        entry = {
            .position = *pos, .size = encoded.bytes_written, .occupied = true};
        std::copy_n(bytes.begin(), encoded.bytes_written, entry.bytes.begin());
        if (state_.unique_packets != UINT64_MAX)
            ++state_.unique_packets;
        (void)state_.all.note(*pos, at);
    } else if ((state_.all.observed && state_.all.next > *pos
                   && state_.all.next - *pos > capacity)
        || !entry.occupied || entry.position != *pos
        || entry.size != encoded.bytes_written
        || !std::equal(bytes.begin(), bytes.begin() + encoded.bytes_written,
            entry.bytes.begin())) {
        if (state_.ignored_copies != UINT64_MAX)
            ++state_.ignored_copies;
        return;
    }
    const auto bit = path == DataPath::multicast ? 2U : 1U;
    if (entry.paths & bit)
        return;
    entry.paths |= bit;
    if (path == DataPath::multicast)
        entry.multicast_at_us = at;
    if (!unique && state_.matched_copies != UINT64_MAX)
        ++state_.matched_copies;
    (void)(path == DataPath::multicast ? state_.multicast : state_.unicast)
        .note(*pos, at);
}
std::optional<MulticastCoverage> ReceiveProgress::coverage(std::uint64_t begin,
    std::uint64_t through, std::uint64_t origin) const noexcept
{
    if (!state_.valid || begin < origin || through <= begin
        || through - begin > capacity || !state_.all.observed)
        return {};
    const auto first = begin - origin, last = through - origin;
    if (last > state_.all.next
        || (state_.all.next > first && state_.all.next - first > capacity))
        return {};
    MulticastCoverage result {UINT64_MAX, 0};
    for (auto p = first; p < last; ++p) {
        const auto& entry = entries_[p % capacity];
        if (!entry.occupied || entry.position != p || !(entry.paths & 2U))
            return {};
        result.first_at_us =
            std::min(result.first_at_us, entry.multicast_at_us);
        result.last_at_us = std::max(result.last_at_us, entry.multicast_at_us);
    }
    return result;
}
std::optional<ReceiveDataProgress> ReceiveProgress::snapshot(
    std::uint64_t origin) const noexcept
{
    if (!state_.valid || floor_ > UINT64_MAX - origin)
        return {};
    auto s = state_;
    s.origin = origin;
    s.window_floor = origin + floor_;
    for (auto* water : {&s.all, &s.unicast, &s.multicast}) {
        if (water->next > UINT64_MAX - origin)
            return {};
        water->next += origin;
    }
    return s;
}
}
