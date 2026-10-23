#pragma once

#include "compiled_filter.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/packet_filter.hpp"

#include <optional>

namespace robotweax::srt::extensions {

inline constexpr std::uint16_t member_filter_subtype = 0x7f30;

struct MemberFilterReceiveResult {
    Error error = Error::none;
    bool admitted = false;
    std::span<const PacketView> recovered {};
};

// Initial individual-socket cleartext binding. The generic dispatcher keeps
// group/receiver ownership independent of this particular wire adapter.
class MemberFilterRuntime {
public:
    static constexpr std::size_t maximum_outputs = 4;
    static constexpr std::size_t inline_state = 4096;
    static constexpr std::size_t maximum_state = 128 * 1024;
    static constexpr std::size_t maximum_symbol =
        packet_header_size + maximum_data_payload_size;
    static constexpr std::size_t maximum_datagram = 1500;
    [[nodiscard]] Error start(const CompiledFilter& filter,
        std::uint32_t local_socket, std::uint32_t peer_socket,
        SequenceNumber send_initial, SequenceNumber receive_initial,
        std::size_t send_capacity, std::size_t receive_capacity,
        std::size_t maximum_payload) noexcept;
    [[nodiscard]] Error source_sent(const PacketView& packet,
        SequenceNumber first, std::uint64_t now) noexcept;
    [[nodiscard]] MemberFilterReceiveResult receive_source(
        const PacketView& packet, SequenceNumber first,
        std::uint64_t now) noexcept;
    [[nodiscard]] MemberFilterReceiveResult receive_repair(
        const PacketView& packet, SequenceNumber first,
        std::uint64_t now) noexcept;
    [[nodiscard]] Error poll_sender(
        SequenceNumber first, std::uint64_t now) noexcept;
    [[nodiscard]] std::optional<std::span<const std::byte>>
    pending() const noexcept;
    void consume() noexcept;

private:
    struct Direction {
        alignas(std::max_align_t) std::array<std::byte, inline_state> state {};
        std::unique_ptr<std::byte[]> expanded_state;
        std::array<FilterOutput, maximum_outputs> outputs {};
        std::array<std::byte, maximum_outputs * maximum_symbol> bytes {};
        std::array<PathBudget, 1> paths {};
        FilterEnvironment environment {};
        SequenceNumber first;
        FilterInstance instance; // Destroy before the storage it borrows.
    };
    struct Datagram {
        // Encoders initialize the complete used prefix. Only that prefix may
        // be copied/submitted; unused temporary capacity is never observed.
        std::array<std::byte, maximum_datagram> bytes;
        std::size_t size = 0;
    };
    [[nodiscard]] Error advance(Direction& direction, SequenceNumber first,
        std::uint64_t now, bool sender) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> position(
        const Direction& direction, SequenceNumber sequence) const noexcept;
    [[nodiscard]] Error queue(const FilterDispatchResult& result) noexcept;
    [[nodiscard]] MemberFilterReceiveResult recover(
        const FilterDispatchResult& result) noexcept;
    [[nodiscard]] bool valid_source(
        const PacketView& packet, bool sender) const noexcept;
    [[nodiscard]] MemberFilterReceiveResult receive_symbol(FilterInputKind kind,
        SequenceNumber sequence, std::span<const std::byte> symbol,
        SequenceNumber first, std::uint64_t now) noexcept;
    Direction sender_;
    Direction receiver_;
    std::array<Datagram, maximum_outputs> pending_ {};
    std::array<PacketView, maximum_outputs> recovered_ {};
    std::size_t head_ = 0;
    std::size_t pending_count_ = 0;
    std::size_t credit_ = 0;
    std::size_t maximum_payload_ = 0;
    std::uint32_t local_socket_ = 0;
    std::uint32_t peer_socket_ = 0;
    Error failure_ = Error::none;
    bool started_ = false;
};

} // namespace robotweax::srt::extensions
