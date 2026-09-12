#pragma once

#include "robotweax/srt/crypto.hpp"
#include "robotweax/srt/error.hpp"
#include "robotweax/srt/packet.hpp"
#include "robotweax/srt/reliability.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace robotweax::srt {

inline constexpr std::size_t maximum_loss_words_per_packet =
    maximum_data_payload_size / sizeof(std::uint32_t);

/**
 * Non-owning send-buffer view. `payload` remains valid only until the owning
 * SendBuffer is next modified or destroyed.
 */
struct OutboundPacket {
    DataHeader header{};
    std::span<const std::byte> payload{};
};

struct SendDropResult {
    std::size_t packets = 0;
    std::size_t bytes = 0;
    std::uint32_t first_message_number = 0;
    SequenceRange sequences{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return packets != 0U;
    }
};

struct StreamEnqueueResult {
    Error error = Error::none;
    std::size_t bytes_accepted = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return error == Error::none;
    }
};

/**
 * Fixed-capacity packet store. Construction allocates its bounded storage and
 * throws `std::invalid_argument` for invalid dimensions or `std::bad_alloc` on
 * allocation failure. Later operations are allocation-free after their
 * construction-time queues have been sized. One instance is not internally
 * synchronized.
 */
class SendBuffer {
public:
    SendBuffer(SequenceNumber initial_sequence, std::size_t capacity_packets,
        std::size_t maximum_payload_size = maximum_data_payload_size);

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t size() const noexcept { return occupied_count_; }
    [[nodiscard]] std::size_t sequence_span() const noexcept
    {
        return sequence_span_;
    }
    [[nodiscard]] std::size_t available() const noexcept
    {
        return capacity() - sequence_span_;
    }
    [[nodiscard]] std::size_t packets_in_flight() const noexcept { return packets_in_flight_; }
    [[nodiscard]] bool has_pending_retransmission() noexcept;
    [[nodiscard]] SequenceNumber first_sequence() const noexcept { return first_sequence_; }
    [[nodiscard]] SequenceNumber next_sequence() const noexcept
    {
        return first_sequence_.advanced(
            static_cast<std::uint32_t>(sequence_span_));
    }
    [[nodiscard]] bool synchronize_empty(
        SequenceNumber next_sequence) noexcept;

    [[nodiscard]] Error enqueue_message(
        std::span<const std::byte> message,
        std::uint32_t message_number,
        PacketTimestamp timestamp,
        std::uint32_t destination_socket_id,
        bool in_order = true,
        std::uint64_t enqueue_microseconds = 0,
        std::uint64_t expiration_microseconds = 0) noexcept;
    [[nodiscard]] StreamEnqueueResult enqueue_stream(
        std::span<const std::byte> bytes,
        std::uint32_t message_number,
        PacketTimestamp timestamp,
        std::uint32_t destination_socket_id,
        std::uint64_t enqueue_microseconds = 0) noexcept;

    [[nodiscard]] std::optional<OutboundPacket> next_packet() noexcept;
    // Converts a sent slot into its immutable encrypted wire form in place.
    // Repeating the call is valid only for the same selector and ciphertext.
    [[nodiscard]] Error preserve_encrypted_payload(
        SequenceNumber sequence,
        EncryptionKey key,
        std::span<const std::byte> ciphertext) noexcept;
    // Replaces the clear slot body with its exact immutable protected wire
    // representation. For GCM this is ciphertext followed by the complete
    // authentication tag; repeat calls must match every stored byte.
    [[nodiscard]] Error preserve_protected_payload(SequenceNumber sequence,
        EncryptionKey key, CryptoMode mode,
        std::span<const std::byte> protected_payload) noexcept;
    [[nodiscard]] Error acknowledge_before(SequenceNumber sequence) noexcept;
    // Validates the complete sequence range without changing retransmission
    // or drop-request state. Every current packet must already have appeared
    // on the wire, or be a retained tombstone for a previously dropped
    // message.
    [[nodiscard]] Error validate_retransmission_range(
        SequenceRange range) const noexcept;
    [[nodiscard]] Error request_retransmission(
        SequenceRange range,
        std::size_t* newly_queued_packets = nullptr,
        std::size_t* newly_queued_bytes = nullptr) noexcept;
    [[nodiscard]] bool request_retransmission_of_last_sent() noexcept;
    [[nodiscard]] std::size_t request_retransmission_of_all_sent() noexcept;
    [[nodiscard]] std::optional<SendDropResult>
    next_pending_drop_request() noexcept;
    [[nodiscard]] bool has_pending_drop_request() noexcept;
    // Queues sequence-only DROPREQ replies for NAK ranges that predate the
    // sender buffer. Capacity validation is transactional.
    [[nodiscard]] bool queue_range_drop_requests(
        std::span<const SequenceRange> ranges) noexcept;
    [[nodiscard]] SendDropResult drop_messages_older_than(
        std::uint64_t cutoff_microseconds) noexcept;
    [[nodiscard]] SendDropResult drop_expired_message(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] std::size_t buffered_payload_bytes() const noexcept;
    [[nodiscard]] std::uint64_t
    buffered_span_milliseconds() const noexcept;

private:
    struct Slot {
        DataHeader header{};
        std::array<std::byte, maximum_data_payload_size> payload{};
        std::uint16_t payload_size = 0;
        std::uint16_t plaintext_size = 0;
        CryptoMode protection_mode = CryptoMode::automatic;
        bool occupied = false;
        // Expired messages remain as lightweight sequence tombstones until
        // cumulative ACK. A repeated NAK can therefore trigger DROPREQ again.
        bool dropped = false;
        bool drop_request_queued = false;
        bool sent = false;
        bool retransmission_queued = false;
        std::uint64_t enqueue_microseconds = 0;
        std::uint64_t expiration_microseconds = 0;
    };

    [[nodiscard]] Slot* find(SequenceNumber sequence) noexcept;
    void discard_slot(Slot& slot, bool retain_drop_marker = false) noexcept;
    void refresh_first_buffered_enqueue_time() noexcept;
    void refresh_buffered_enqueue_time_bounds() noexcept;
    void compact_retransmission_queue() noexcept;
    void compact_drop_request_queue() noexcept;
    [[nodiscard]] bool queue_drop_request(SequenceNumber sequence) noexcept;
    [[nodiscard]] bool queue_retransmission(SequenceNumber sequence) noexcept;

    std::vector<Slot> slots_;
    std::vector<SequenceNumber> retransmission_queue_;
    std::vector<SequenceNumber> drop_request_queue_;
    std::vector<SequenceRange> range_drop_request_queue_;
    SequenceNumber first_sequence_;
    std::size_t maximum_payload_size_ = maximum_data_payload_size;
    std::size_t head_ = 0;
    // The wire-sequence span includes retained drop tombstones, whereas
    // occupied_count_ counts packets that can still be sent or retransmitted.
    std::size_t sequence_span_ = 0;
    std::size_t occupied_count_ = 0;
    std::size_t buffered_plaintext_bytes_ = 0;
    std::size_t expiring_packet_count_ = 0;
    std::uint64_t first_buffered_enqueue_microseconds_ = 0;
    std::uint64_t last_buffered_enqueue_microseconds_ = 0;
    std::size_t retransmission_head_ = 0;
    std::size_t retransmission_size_ = 0;
    std::size_t drop_request_head_ = 0;
    std::size_t drop_request_size_ = 0;
    std::size_t range_drop_request_head_ = 0;
    std::size_t range_drop_request_size_ = 0;
    std::size_t packets_in_flight_ = 0;
};

} // namespace robotweax::srt
