// SPDX-License-Identifier: MIT
#pragma once
#include "robotweax/srt/codec.hpp"
#include "compat/hybrid/hybrid_control.hpp"
#include <array>
#include <optional>

namespace robotweax::srt::compat::hybrid {
// Explicit host assertions, not activity inferred from admissions or silence.
struct ActivityObservation {
    Activity state = Activity::startup;
    std::uint64_t produced_next = 0, at_us = 0, expires_at_us = 0;
};
// A high water is neither contiguous delivery nor an ACK horizon. Only a new
// position advances its timestamp; repeated/older copies cannot renew progress.
struct DataHighWater {
    bool observed = false;
    std::uint64_t next = 0, packets = 0, last_advance_us = 0;
    [[nodiscard]] bool note(std::uint64_t position, std::uint64_t at) noexcept;
};
enum class DataPath { unicast, multicast };
struct ReceiveDataProgress {
    std::uint64_t route_epoch = 0;
    bool valid = true;
    std::uint64_t origin = 0, window_floor = 0, unique_packets = 0,
                  matched_copies = 0, ignored_copies = 0;
    DataHighWater all, unicast, multicast;
};
// Serialized with the core runtime. Exact accepted plaintext/header copies,
// bounded to32 recent positions, allow a late path copy after application pop.
// No hash/MAC is invented; crypto and provenance validation precede this hook.
struct MulticastCoverage {
    std::uint64_t first_at_us = 0, last_at_us = 0, route_epoch = 0;
};
class ReceiveProgress {
public:
    static constexpr std::size_t capacity = 32;
    explicit ReceiveProgress(SequenceNumber origin) noexcept
        : sequence_(origin)
    {
    }
    void sync_floor(SequenceNumber) noexcept;
    // Preserve canonical/UC history and high waters; old MC copies cannot prove
    // coverage after a membership loss. New matching MC copies may fill it.
    void invalidate_multicast_coverage() noexcept;
    [[nodiscard]] std::optional<MulticastCoverage> coverage(std::uint64_t begin,
        std::uint64_t through, std::uint64_t origin) const noexcept;
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
        std::uint64_t multicast_at_us = 0;
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
    // Permanent native multicast failure remains true after explicit UC recovery.
    // This does not describe the surviving primary UC session's health.
    bool source_failed = false, unicast_only = false;
    Mode delivery_mode = Mode::multicast;
    std::optional<ActivityObservation> activity;
};
}
