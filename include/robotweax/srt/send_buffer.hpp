#pragma once

#include "robotweax/srt/crypto.hpp"
#include "robotweax/srt/detail/range_drop_queue.hpp"
#include "robotweax/srt/error.hpp"
#include "robotweax/srt/packet.hpp"
#include "robotweax/srt/payload_pool.hpp"
#include "robotweax/srt/reliability.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
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
    std::uint64_t sequence_position = 0;
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
    SendBuffer(const SendBuffer&) = default;
    SendBuffer(SendBuffer&&) noexcept = default;
    SendBuffer& operator=(SendBuffer&&) noexcept = default;
    SendBuffer& operator=(const SendBuffer& other);

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t maximum_payload_size() const noexcept
    {
        return maximum_payload_size_;
    }
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

    // now_microseconds records when an original packet is first selected
    // for transmission; sender too-late drop measures age from that time.
    [[nodiscard]] std::optional<OutboundPacket> next_packet(
        bool defer_retransmission_commit = false,
        bool allow_retransmission = true,
        std::uint64_t now_microseconds = 0) noexcept;
    [[nodiscard]] std::optional<OutboundPacket>
    peek_retransmission_packet() noexcept;
    void requeue_prepared_retransmission(SequenceNumber sequence) noexcept;
    [[nodiscard]] std::optional<OutboundPacket>
    peek_new_packet() const noexcept;
    // A prepared UDP retry must not revive an ACKed or expired packet.
    [[nodiscard]] bool retains_packet(SequenceNumber sequence) const noexcept;
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
    [[nodiscard]] Error request_retransmission(SequenceRange range,
        std::size_t* newly_queued_packets = nullptr,
        std::size_t* newly_queued_bytes = nullptr,
        std::uint64_t now_microseconds = 0,
        std::uint64_t minimum_repeat_microseconds = 0) noexcept;
    // Commit the repeat clock only after the retransmission reached the wire.
    void note_retransmission_sent(
        SequenceNumber sequence, std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] bool request_retransmission_of_last_sent() noexcept;
    [[nodiscard]] std::size_t request_retransmission_of_all_sent() noexcept;
    [[nodiscard]] std::optional<SendDropResult>
    next_pending_drop_request() noexcept;
    [[nodiscard]] bool has_pending_drop_request() noexcept;
    // Requeues one DROPREQ for every retained message tombstone. This is used
    // by delivery profiles that cannot rely on a receiver NAK to recover a
    // lost DROPREQ.
    [[nodiscard]] std::size_t queue_retained_drop_requests() noexcept;
    [[nodiscard]] bool has_retained_drop() const noexcept;
    // A no-ARQ owner can move this contiguous tombstone prefix into its
    // bounded retirement state, freeing payload slots for fresh DATA.
    [[nodiscard]] std::optional<SequenceRange> retired_prefix() const noexcept;
    void release_retired_prefix() noexcept;
    // Conservative wakeup bound; acknowledgement can leave an earlier bound
    // until the next expiry scan refines it. Never later than an active TTL.
    [[nodiscard]] std::optional<std::uint64_t>
    next_expiration_microseconds() const noexcept;
    // Exposes the stale-entry compaction of the retransmission ring, which
    // the public queueing paths reach only when the ring is full.
    void compact_retransmission_queue_for_testing() noexcept
    {
        compact_retransmission_queue();
    }
    // Queues sequence-only DROPREQ replies for NAK ranges that predate the
    // sender buffer. Overlapping/adjacent coverage is coalesced, with at most
    // one packet worth of disjoint ranges plus a rollover split. Admission
    // validates the complete batch transactionally and never grows storage.
    [[nodiscard]] bool queue_range_drop_requests(
        std::span<const SequenceRange> ranges) noexcept;
    [[nodiscard]] SendDropResult drop_messages_older_than(
        std::uint64_t cutoff_microseconds,
        std::uint64_t now_microseconds = 0U) noexcept;
    [[nodiscard]] SendDropResult drop_expired_message(
        std::uint64_t now_microseconds) noexcept;
    [[nodiscard]] std::size_t buffered_payload_bytes() const noexcept;
    [[nodiscard]] std::uint64_t
    buffered_span_milliseconds() const noexcept;

private:
    struct LiveMetadata {
        std::uint64_t last_retransmission_send_microseconds = 0;
        std::uint64_t expiration_microseconds = 0;
    };
    struct DropMetadata {
        SequenceNumber message_first {};
        SequenceNumber message_last {};
        std::size_t remaining_bytes = 0;
    };
    // Dropped slots no longer need retransmission or expiration timestamps.
    // The existing dropped flag discriminates these mutually exclusive states.
    union SlotMetadata {
        LiveMetadata live;
        DropMetadata dropped;
        SlotMetadata() noexcept
            : live {}
        {
        }
    };
    struct Slot {
        DataHeader header{};
        std::uint32_t payload_index = 0;
        std::uint64_t sequence_position = 0;
        std::uint64_t enqueue_microseconds = 0;
        std::uint64_t first_send_microseconds = 0;
        SlotMetadata metadata;
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
        bool has_retransmission_send_time = false;
    };
    static_assert(std::is_trivially_copyable_v<Slot>);
    static_assert(sizeof(Slot) <= 80U, "sender slot metadata budget exceeded");

    [[nodiscard]] Slot* find(SequenceNumber sequence) noexcept;
    void discard_slot(Slot& slot, bool retain_drop_marker = false) noexcept;
    void refresh_first_buffered_enqueue_time() noexcept;
    void refresh_buffered_enqueue_time_bounds() noexcept;
    void compact_retransmission_queue() noexcept;
    void compact_drop_request_queue() noexcept;
    [[nodiscard]] bool queue_drop_request(SequenceNumber sequence) noexcept;
    [[nodiscard]] bool queue_retransmission(SequenceNumber sequence) noexcept;

    // Fenwick counts change only on tombstone creation/removal, not DATA sends.
    [[nodiscard]] std::size_t dropped_prefix(std::size_t end) const noexcept;
    std::vector<std::uint32_t> dropped_counts_;
    detail::PayloadPool payloads_;
    std::vector<Slot> slots_;
    std::vector<SequenceNumber> retransmission_queue_;
    std::vector<SequenceNumber> drop_request_queue_;
    detail::RangeDropQueue range_drop_request_queue_;
    SequenceNumber first_sequence_;
    std::uint64_t next_sequence_position_ = 0;
    std::size_t maximum_payload_size_ = maximum_data_payload_size;
    std::size_t head_ = 0;
    // The wire-sequence span includes retained drop tombstones, whereas
    // occupied_count_ counts packets that can still be sent or retransmitted.
    std::size_t sequence_span_ = 0;
    // Offset from head_ to the first slot not yet examined for an original
    // send. Earlier occupied slots have been sent; drops remain tombstones.
    // Appending preserves this cursor, while removing a prefix rebases it.
    std::size_t next_unsent_offset_ = 0;
    std::size_t occupied_count_ = 0;
    std::size_t retained_drop_count_ = 0;
    std::size_t buffered_plaintext_bytes_ = 0;
    std::size_t expiring_packet_count_ = 0;
    // Lower bound of every buffered expiration. Lets the per-poll expiry
    // check return without a scan until the earliest TTL can have elapsed.
    std::uint64_t earliest_expiration_microseconds_ = 0;
    std::uint64_t first_buffered_enqueue_microseconds_ = 0;
    std::uint64_t last_buffered_enqueue_microseconds_ = 0;
    std::size_t retransmission_head_ = 0;
    std::size_t retransmission_size_ = 0;
    std::size_t drop_request_head_ = 0;
    std::size_t drop_request_size_ = 0;
    std::size_t packets_in_flight_ = 0;
};

} // namespace robotweax::srt
