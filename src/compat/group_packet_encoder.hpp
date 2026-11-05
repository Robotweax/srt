#pragma once

#include "robotweax/srt/crypto.hpp"
#include "robotweax/srt/udp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace robotweax::srt::compat {

// One canonical original-packet producer per dispatch stream incarnation.
// This does not announce keys, commit member history or submit datagrams.
// Its owner must coordinate those operations before committing a prepared
// packet, and must retain the existing per-member KM exchange.
class GroupPacketEncoder {
    struct Generation;

public:
    static constexpr std::size_t maximum_packets = 256;
    static constexpr std::size_t maximum_generations = 4;
    static constexpr std::size_t maximum_datagram_size =
        packet_header_size + maximum_data_payload_size;

    struct Configuration {
        std::uint32_t group_id = 0;
        std::uint64_t stream_generation = 0;
        SequenceNumber initial_sequence {};
        IpAddressFamily family = IpAddressFamily::ipv4;
        std::size_t maximum_transmission_unit = 1500;
        std::size_t packet_capacity = 64;
        std::size_t generation_capacity = maximum_generations;
        // Null selects plaintext. Otherwise this is the same initialized
        // schedule used by all member sessions, with its effective CTR/GCM mode.
        std::shared_ptr<SharedTransmitKeySchedule> key_schedule;
    };

    class Packet {
    public:
        Packet(const Packet&) = delete;
        Packet& operator=(const Packet&) = delete;
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept
        {
            return std::span {bytes_}.first(size_);
        }
        [[nodiscard]] const DataHeader& header() const noexcept
        {
            return header_;
        }
        [[nodiscard]] std::uint64_t stream_generation() const noexcept
        {
            return stream_generation_;
        }
        [[nodiscard]] std::optional<std::uint64_t>
        crypto_generation() const noexcept;
        [[nodiscard]] bool uses_key_schedule(
            const std::shared_ptr<SharedTransmitKeySchedule>&) const noexcept;
        // Pins the immutable cipher generation until the emitter retires it.
        [[nodiscard]] std::shared_ptr<const void>
        generation_owner() const noexcept;

    private:
        friend class GroupPacketEncoder;
        Packet() = default;
        std::array<std::byte, maximum_datagram_size> bytes_ {};
        std::size_t size_ = 0;
        DataHeader header_ {};
        std::uint64_t stream_generation_ = 0;
        std::shared_ptr<Generation> generation_;
    };

    enum class Status : std::uint8_t {
        prepared,
        invalid_packet,
        wrong_sequence,
        conflicting_packet,
        full,
        failed,
        allocation_failure,
        closed,
    };
    struct Result {
        Status status = Status::invalid_packet;
        Error error = Error::none;
        std::shared_ptr<const Packet> packet;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return status == Status::prepared;
        }
    };
    struct Snapshot {
        SequenceNumber next_sequence {};
        std::size_t live_packets = 0;
        std::size_t cached_generations = 0;
        std::uint64_t committed_packets = 0;
        std::uint64_t unused_packets = 0;
        bool prepared = false;
        bool closed = false;
        Error failure = Error::none;
    };

    // Provider is borrowed and must outlive the encoder AND all returned
    // packet/generation leases. Provider callbacks must not reenter this encoder.
    // Creation never initializes or replaces a schedule's keys. The owner
    // supplies the actual next stream sequence; its resolved position must
    // match the schedule's live frontier rather than rewind it.
    [[nodiscard]] static std::unique_ptr<GroupPacketEncoder> create(
        Configuration, CryptoProvider& = default_crypto_provider()) noexcept;
    ~GroupPacketEncoder();
    GroupPacketEncoder(const GroupPacketEncoder&) = delete;
    GroupPacketEncoder& operator=(const GroupPacketEncoder&) = delete;

    // Caller supplies the original identity with selector NONE; this encoder
    // selects its shared generation. Exactly one preparation is pending.
    // Identical retries return the same object without resealing; conflicting
    // identities/payloads cannot reuse that sequence/nonce. There is no abort
    // or rewind. Commit only after the coordinator's acceptance transaction.
    [[nodiscard]] Result prepare(
        const DataHeader&, std::span<const std::byte>) noexcept;
    [[nodiscard]] bool commit(const std::shared_ptr<const Packet>&) noexcept;
    // Empty-recipient timeline advance, with no seal or pending preparation.
    [[nodiscard]] bool advance_unused() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;
    void close() noexcept;

private:
    explicit GroupPacketEncoder(
        Configuration, CryptoProvider&, CryptoPayloadBudget);
    [[nodiscard]] std::shared_ptr<Generation> generation_locked(
        std::uint64_t, Error&);

    mutable std::mutex mutex_;
    Configuration configuration_;
    CryptoProvider& provider_;
    CryptoPayloadBudget budget_;
    Snapshot state_;
    std::uint64_t next_position_ = 0;
    std::array<std::weak_ptr<const Packet>, maximum_packets> packets_;
    std::array<std::shared_ptr<Generation>, maximum_generations> generations_;
    std::shared_ptr<const Packet> pending_;
    std::array<std::byte, maximum_data_payload_size> pending_plaintext_ {};
    std::size_t pending_plaintext_size_ = 0;
};

} // namespace robotweax::srt::compat
