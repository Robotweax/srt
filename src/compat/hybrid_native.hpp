// SPDX-License-Identifier: MIT
#pragma once
#include "compat/hybrid_conclusions.hpp"
#include "compat/hybrid_exchange.hpp"
#include "compat/hybrid_progress.hpp"
#include "compat/runtime_scheduler.hpp"
#include <atomic>
#include <mutex>
#include <optional>

namespace robotweax::srt::compat {
class ConnectionRuntime;
struct HybridAutomaticTestAccess;
}
namespace robotweax::srt::compat::hybrid {
// Provisional local assignment, default-OFF. No public C selector or automatic
// switching. Native handshakes still reject authentication version 7.
inline constexpr std::uint16_t exchange_subtype = 0x7f17;
inline constexpr std::size_t maximum_native_control_size =
    packet_header_size + exchange_size;
[[nodiscard]] bool native_candidate(const PacketView&) noexcept;
[[nodiscard]] bool decode_exchange_packet(
    const PacketView&, ExchangeRecord&) noexcept;
[[nodiscard]] bool encode_exchange_packet(std::span<const std::byte> body,
    PacketTimestamp, std::uint32_t destination, std::span<std::byte>) noexcept;

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
class CoordinatorBridge;
class ReceiverBridge;
class MembershipRecovery;
struct NativeOptions {
    Capability policy;
    CredentialId credential_id {};
    std::uint64_t stream = 0, member = 0, initial_position = 0;
    std::uint32_t retry_interval_us = 100'000, timeout_us = 1'000'000,
                  maximum_sends_per_stage = 4;
    // All callbacks receive owner.get(). They must be nonblocking; use weak
    // references back to the service/runtime to avoid an ownership cycle.
    std::shared_ptr<void> owner;
    CredentialProvider::Resolve resolve = nullptr;
    CredentialProvider::Revalidate revalidate = nullptr;
    using Enroll = bool (*)(
        void*, const Context&, const Authorization&, std::uint64_t) noexcept;
    Enroll enroll = nullptr;
};
class NativeService;
[[nodiscard]] std::shared_ptr<NativeService> attach_native_control(
    const std::shared_ptr<ConnectionRuntime>&, NativeOptions) noexcept;

// One reserved scheduler service, bounded queues and a single protocol owner.
// Credential/enrollment callbacks never run under runtime/group/queue locks.
class NativeService {
public:
    enum class Failure {
        none,
        closed,
        lifetime,
        options,
        scheduler,
        exchange,
        enrollment,
        transport,
        events_full
    };
    enum class Admission { accepted, not_ready, full, invalid, closed };
    struct Event {
        enum class Kind { received, submitted, retryable, rejected, expired };
        Kind kind = Kind::rejected;
        Session::Result received;
        Message message;
        std::uint64_t generation = 0;
    };
    struct Snapshot {
        bool active = false, closed = false;
        Failure failure = Failure::none;
        std::uint64_t generation = 0, submitted = 0, retryable = 0,
                      rejected = 0, dropped = 0, received = 0;
        std::uint64_t expires_at_us = UINT64_MAX;
        std::size_t queued = 0, events = 0;
        Context context;
    };
    struct Configuration {
        bool caller = false;
        std::uint32_t local_socket = 0, peer_socket = 0;
        std::shared_ptr<const Conclusions> conclusions;
        Exchange::Configuration exchange;
        NativeOptions callbacks;
        std::shared_ptr<void> transport_owner;
        bool (*bound)(void*, const NativeService*) noexcept = nullptr;
        UdpIoResult (*transmit)(void*, const NativeService*,
            std::span<const std::byte>, std::uint64_t) noexcept = nullptr;
        using Clock = std::uint64_t (*)(void*) noexcept;
        Clock clock = nullptr;
        void* clock_context = nullptr;
        std::uint64_t affinity = 0;
        std::uint64_t timestamp_origin_us = 0;
    };
    static constexpr std::size_t capacity = 16, event_capacity = 8, batch = 8;
    [[nodiscard]] static std::shared_ptr<NativeService> create(Configuration,
        std::shared_ptr<RuntimeScheduler>,
        CryptoProvider& = default_crypto_provider()) noexcept;
    [[nodiscard]] static std::shared_ptr<NativeService> create_for_testing(
        Configuration, CryptoProvider& = default_crypto_provider()) noexcept;
    ~NativeService();
    [[nodiscard]] bool activate() noexcept;
    [[nodiscard]] bool enqueue(
        const PacketView&, std::uint64_t arrival_us) noexcept;
    // Reports require observation time and age_us=0; age is calculated at seal.
    // Use a fresh serial for each report attempt. Commands can retry identical
    // serial/body; every queued attempt has an exclusive <=5-second deadline.
    [[nodiscard]] Admission send(const Message&, std::uint64_t not_after_us,
        std::optional<std::uint64_t> observed_at_us = std::nullopt) noexcept;
    [[nodiscard]] bool pop(Event&) noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;
    void close(Failure = Failure::closed) noexcept;
    [[nodiscard]] bool closed() const noexcept
    {
        return cancelled_.load();
    }
    [[nodiscard]] const std::shared_ptr<const Conclusions>&
    origin() const noexcept
    {
        return configuration_.conclusions;
    }
    void pump_for_testing() noexcept;
    [[nodiscard]] static std::uint64_t monotonic_now() noexcept;

private:
    friend std::shared_ptr<NativeService> attach_native_control(
        const std::shared_ptr<ConnectionRuntime>&, NativeOptions) noexcept;
    friend class CoordinatorBridge;
    friend class ReceiverBridge;
    friend class MembershipRecovery;
    friend struct robotweax::srt::compat::HybridAutomaticTestAccess;
    [[nodiscard]] bool bridge_work(CoordinatorBridge&) noexcept;
    [[nodiscard]] bool receiver_work(ReceiverBridge&) noexcept;
    [[nodiscard]] Admission send_impl(const Message&, std::uint64_t,
        std::optional<std::uint64_t>, bool bridge) noexcept;
    struct Work {
        std::weak_ptr<NativeService> owner;
    };
    struct Job {
        bool packet = false;
        std::array<std::byte, maximum_native_control_size> bytes {};
        std::size_t size = 0;
        Message message;
        std::uint64_t arrival = 0, not_after = 0;
        std::optional<std::uint64_t> observed;
    };
    NativeService(Configuration, CryptoProvider&);
    [[nodiscard]] static bool valid(const Configuration&) noexcept;
    static void run(void*) noexcept;
    void pump() noexcept;
    void cleanup() noexcept;
    [[nodiscard]] std::uint64_t now() const noexcept;
    [[nodiscard]] bool live() noexcept;
    [[nodiscard]] bool ensure_active() noexcept;
    void handle(const Exchange::Step&) noexcept;
    void process(const Job&) noexcept;
    void transmit(
        std::span<const std::byte>, std::uint64_t, const Message*) noexcept;
    void publish(Event) noexcept;
    [[nodiscard]] bool wake() noexcept;
    Configuration configuration_;
    Exchange exchange_;
    std::optional<Session> session_;
    std::shared_ptr<RuntimeScheduler> scheduler_;
    RuntimeScheduler::ServiceToken service_;
    std::shared_ptr<Work> work_;
    mutable std::mutex queue_mutex_;
    std::mutex protocol_mutex_;
    std::array<Job, capacity> jobs_ {};
    std::array<Event, event_capacity> events_ {};
    std::size_t head_ = 0, size_ = 0, event_head_ = 0, event_size_ = 0;
    Snapshot state_;
    std::atomic_bool cancelled_ = false;
    std::atomic<std::uint64_t> expires_at_ = UINT64_MAX;
    bool activated_ = false, started_ = false, manual_ = false;
    bool bridged_ = false, application_used_ = false;
    std::weak_ptr<ConnectionRuntime> runtime_;
    std::uint64_t setup_deadline_ = 0;
    std::array<std::uint64_t, 3> last_report_ {};
};
#endif
} // namespace robotweax::srt::compat::hybrid
