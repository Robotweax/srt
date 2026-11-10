#pragma once

#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/connection_group.hpp"
#include "robotweax/srt/session_authentication.hpp"

#include <array>

namespace robotweax::srt::compat::hybrid {
// Provisional component protocol. No handshake extension ID or public socket
// option selects it; the production handshake continues to reject auth v7.
inline constexpr std::uint32_t version = 1;
inline constexpr std::uint16_t control_subtype = 0x7f16;
inline constexpr std::uint8_t authentication_version = 7;
inline constexpr std::size_t capability_size = 48, context_size = 192,
                             message_size = 96;
inline constexpr std::uint32_t originals = 1, prefix_replay = 2, progress = 4,
                               overlap_control = 8, source_recovery = 16,
                               policy_query = 32, return_control = 64,
                               startup_fallback = 128, core_features = 7,
                               legacy_features = 31, policy_query_features = 63,
                               return_features = 127, known_features = 255;
inline constexpr std::uint32_t maximum_prefix = 256,
                               maximum_interval_us = 1'000'000,
                               maximum_age_us = 5'000'000;
enum class CapabilityKind : std::uint32_t { offer = 1, selection = 2 };
struct Capability {
    CapabilityKind kind = CapabilityKind::offer;
    std::uint32_t supported = core_features, required = core_features,
                  prefix_limit = maximum_prefix;
    std::uint64_t stream = 0, member = 0, initial_position = 0;
    friend bool operator==(const Capability&, const Capability&) = default;
};
[[nodiscard]] bool valid(const Capability&) noexcept;
[[nodiscard]] bool encode(const Capability&, std::span<std::byte>) noexcept;
[[nodiscard]] bool decode(std::span<const std::byte>, Capability&) noexcept;
[[nodiscard]] bool negotiate(const Capability& peer_offer,
    const Capability& local_policy, std::uint64_t stream, std::uint64_t member,
    std::uint64_t initial_position, Capability& selection) noexcept;
struct Context {
    MulticastDispatch dispatch {};
    MulticastSource source {};
    Capability offer {}, selection {};
    friend bool operator==(const Context&, const Context&) = default;
};
[[nodiscard]] bool valid(const Context&) noexcept;
[[nodiscard]] bool encode(const Context&, std::span<std::byte>) noexcept;
[[nodiscard]] bool decode(std::span<const std::byte>, Context&) noexcept;
enum class Kind : std::uint32_t {
    stream_progress = 1,
    path_report = 2,
    fallback_request = 3,
    delivery_commit = 4,
    delivery_status = 5,
    multicast_ready = 6,
    delivery_query = 7
};
enum class Mode : std::uint32_t { multicast, unicast, overlap };
enum class Activity : std::uint32_t {
    startup,
    producing,
    paused,
    ended,
    source_failed
};
enum class Outcome : std::uint32_t {
    ok,
    denied,
    history_unavailable,
    budget,
    stale,
    busy,
    unsupported
};
struct Message {
    Kind kind = Kind::stream_progress;
    std::uint64_t stream = 0, member = 0, serial = 0, revision = 0,
                  next_position = 0;
    std::uint32_t next_sequence = 0;
    Mode mode = Mode::multicast;
    std::uint64_t retained_floor = 0, multicast_next = 0, produced_next = 0;
    Activity activity = Activity::startup;
    Outcome outcome = Outcome::ok;
    std::uint32_t interval_us = 0, age_us = 0;
    // Negotiated return_control: delivery-response multicast_next is the
    // effective boundary; ready uses [multicast_next,next_position) coverage.
    // Negotiated policy_query: delivery responses echo the request serial in
    // wire word 72, which remains produced_next for STREAM_PROGRESS.
    std::uint64_t request_serial = 0;
    friend bool operator==(const Message&, const Message&) = default;
};
[[nodiscard]] bool valid(const Message&) noexcept;
[[nodiscard]] bool valid(const Message&, const Context&) noexcept;
[[nodiscard]] bool sender_is_caller(Kind) noexcept;
[[nodiscard]] bool encode(const Message&, std::span<std::byte>) noexcept;
[[nodiscard]] bool decode(std::span<const std::byte>, Message&) noexcept;

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
// No policy mutation or socket I/O. The owner supplies a separate pairwise
// control credential to SessionAuthentication, verifies both proofs, and keeps
// that object alive. Group traffic keys are never inputs here. Sharing the
// control credential still permits impersonation; this is experimental.
class Session {
public:
    enum class Status {
        accepted,
        duplicate,
        stale,
        conflict,
        invalid,
        authentication_failed,
        closed
    };
    struct Result {
        Status status = Status::invalid;
        Message message {};
        std::uint64_t first_received_us = 0;
    };
    Session(SessionAuthentication&, const Context&, bool caller) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] Status seal(const Message&, PacketTimestamp,
        std::span<std::byte>, std::size_t& written) noexcept;
    [[nodiscard]] Result receive(
        const PacketView& envelope, std::uint64_t now_us) noexcept;
    [[nodiscard]] bool fresh(
        Kind, std::uint64_t now_us, std::uint32_t maximum_age) const noexcept;
    void close() noexcept;

private:
    struct Entry {
        std::uint64_t serial = 0, at = 0;
        std::array<std::byte, message_size> wire {};
    };
    struct Window {
        std::array<Entry, 64> entries {};
        std::uint64_t highest = 0;
        [[nodiscard]] Status classify(
            std::uint64_t, std::span<const std::byte>) const noexcept;
        void commit(
            std::uint64_t, std::span<const std::byte>, std::uint64_t) noexcept;
    };
    struct Report {
        std::uint64_t serial = 0, at = 0;
        std::uint32_t age = 0;
    };
    SessionAuthentication& auth_;
    Context context_;
    std::array<std::byte, context_size> binding_ {};
    Window sent_, received_;
    std::array<Report, 2> reports_ {};
    std::uint64_t last_now_ = 0;
    bool caller_ = false, claimed_ = false, closed_ = false;
};
#endif
} // namespace robotweax::srt::compat::hybrid
