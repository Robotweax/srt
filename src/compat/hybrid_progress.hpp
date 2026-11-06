// SPDX-License-Identifier: MIT
#pragma once
#include "robotweax/srt/codec.hpp"
#include <array>
#include <optional>

namespace robotweax::srt::compat::hybrid {
// A high water is neither contiguous delivery nor an ACK horizon. Only a new
// position advances its timestamp; repeated/older copies cannot renew progress.
struct DataHighWater {
    bool observed = false;
    std::uint64_t next = 0, packets = 0, last_advance_us = 0;
    [[nodiscard]] bool note(std::uint64_t position, std::uint64_t at) noexcept;
};
enum class DataPath { unicast, multicast };
struct ReceiveDataProgress {
    bool valid = true;
    std::uint64_t origin = 0, window_floor = 0, unique_packets = 0,
                  matched_copies = 0, ignored_copies = 0;
    DataHighWater all, unicast, multicast;
};
// Serialized with the core runtime. Exact accepted plaintext/header copies,
// bounded to32 recent positions, allow a late path copy after application pop.
// No hash/MAC is invented; crypto and provenance validation precede this hook.
class ReceiveProgress {
public:
    static constexpr std::size_t capacity = 32;
    explicit ReceiveProgress(SequenceNumber origin) noexcept
        : sequence_(origin)
    {
    }
    void sync_floor(SequenceNumber) noexcept;
    void receive(const PacketView& clear, bool accepted_unique, DataPath,
        std::uint64_t arrival_us) noexcept;
    [[nodiscard]] std::optional<ReceiveDataProgress> snapshot(
        std::uint64_t absolute_origin = 0) const noexcept;

private:
    struct Entry {
        std::array<std::byte, packet_header_size + maximum_data_payload_size>
            bytes {};
        std::uint64_t position = 0;
        std::size_t size = 0;
        unsigned paths = 0;
        bool occupied = false;
    };
    [[nodiscard]] std::optional<std::uint64_t> position(
        SequenceNumber) const noexcept;
    SequenceNumber sequence_;
    std::uint64_t floor_ = 0;
    ReceiveDataProgress state_;
    std::array<Entry, capacity> entries_ {};
};
struct SourceDataProgress {
    std::uint64_t stream = 0, member = 0, revision = 0, member_origin = 0,
                  admitted_next = 0, history_floor = 0, observed_at_us = 0;
    DataHighWater multicast, unicast;
    bool source_failed = false, unicast_only = false;
};
}
