// SPDX-License-Identifier: MIT
#pragma once
#include "compat/hybrid/hybrid_admission.hpp"

namespace robotweax::srt::compat::hybrid {
// Fixed exchange bodies; the optional internal carrier is in hybrid_native.hpp.
inline constexpr std::size_t exchange_size = 176;
enum class ExchangeKind : std::uint32_t {
    offer = 1,
    selection,
    proof,
    receipt
};
struct ExchangeRecord {
    ExchangeKind kind = ExchangeKind::offer;
    std::uint32_t caller_socket = 0, listener_socket = 0;
    CredentialId credential_id {};
    Capability capability {.supported = 0, .required = 0, .prefix_limit = 0};
    SessionAuthenticationParameters authentication {};
};
[[nodiscard]] bool encode(const ExchangeRecord&, std::span<std::byte>) noexcept;
[[nodiscard]] bool decode(std::span<const std::byte>, ExchangeRecord&) noexcept;

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
// Trusted native owner supplies frozen connection provenance, never wire claims.
struct CredentialRequest {
    bool caller = false;
    IpEndpoint established_peer;
    std::uint64_t connection_generation = 0;
    std::uint32_t caller_socket = 0, listener_socket = 0;
    std::uint32_t caller_group = 0, listener_group = 0;
    StreamId stream_id;
    CredentialId credential_id {};
    Context context;
};
struct CredentialProvider {
    // No runtime/group locks may be held across callbacks. Context is borrowed
    // and must outlive Exchange. Resolve runs at most once per attempt.
    using Resolve = bool (*)(void*, const CredentialRequest&, Authorization&,
        std::span<std::byte, 32>) noexcept;
    using Revalidate = bool (*)(void*, const CredentialRequest&,
        const Authorization& expected, Authorization& current) noexcept;
    void* context = nullptr;
    Resolve resolve = nullptr;
    Revalidate revalidate = nullptr;
};
class Exchange {
public:
    struct Configuration {
        MulticastDispatch dispatch;
        MulticastSource source;
        Capability policy;
        CredentialId credential_id {};
        IpEndpoint established_peer;
        std::uint64_t connection_generation = 0;
        // Only Listener chooses stream/member/position. Caller supplies zero.
        std::uint64_t stream = 0, member = 0, initial_position = 0;
        std::uint32_t retry_interval_us = 100'000, timeout_us = 1'000'000;
        std::uint32_t maximum_sends_per_stage = 4;
    };
    enum class State {
        empty,
        awaiting_offer,
        awaiting_selection,
        awaiting_proof,
        awaiting_receipt,
        active,
        closed
    };
    enum class Status { waiting, output, duplicate, ignored, active, closed };
    struct Step {
        Status status = Status::waiting;
        bool has_output = false;
        std::array<std::byte, exchange_size> output {};
    };
    Exchange(CryptoProvider&, bool caller) noexcept;
    ~Exchange();
    Exchange(const Exchange&) = delete;
    Exchange& operator=(const Exchange&) = delete;
    [[nodiscard]] Step start(const Configuration&, CredentialProvider,
        std::span<const std::byte> caller_payload,
        std::span<const std::byte> listener_payload,
        std::uint64_t now_us) noexcept;
    [[nodiscard]] Step receive(
        std::span<const std::byte>, std::uint64_t now_us) noexcept;
    [[nodiscard]] Step tick(std::uint64_t now_us) noexcept;
    // Owner deadline/grant check without consuming a retry emission.
    [[nodiscard]] bool revalidate(std::uint64_t now_us) noexcept
    {
        return live(now_us);
    }
    [[nodiscard]] const Authorization* authorization() const noexcept
    {
        return grant_.generation ? &grant_ : nullptr;
    }
    [[nodiscard]] SessionAuthentication* authentication() noexcept;
    [[nodiscard]] const Context& context() const noexcept
    {
        return request_.context;
    }
    [[nodiscard]] State state() const noexcept
    {
        return state_;
    }
    [[nodiscard]] std::uint64_t next_deadline() const noexcept;
    void close() noexcept;

private:
    [[nodiscard]] bool live(std::uint64_t now) noexcept;
    [[nodiscard]] bool establish(
        const ExchangeRecord&, std::uint64_t now) noexcept;
    [[nodiscard]] bool validate_grant(
        std::uint64_t now, bool confirmed) noexcept;
    [[nodiscard]] bool cache(ExchangeKind,
        const SessionAuthenticationParameters&, std::uint64_t now) noexcept;
    [[nodiscard]] Step emit(std::uint64_t now, Status otherwise) noexcept;
    [[nodiscard]] Step fail() noexcept;
    CryptoProvider& crypto_;
    bool caller_ = false;
    Admission admission_;
    Configuration configuration_;
    CredentialProvider provider_;
    CredentialRequest request_;
    Authorization grant_;
    std::array<std::byte, maximum_conclusion_payload> caller_payload_ {},
        listener_payload_ {};
    std::size_t caller_size_ = 0, listener_size_ = 0;
    std::array<std::byte, exchange_size> cached_ {}, last_peer_ {};
    std::uint64_t last_now_ = 0, deadline_ = 0, next_send_ = 0;
    std::uint32_t sends_ = 0;
    bool cached_valid_ = false, last_peer_valid_ = false;
    State state_ = State::empty;
};
#endif
} // namespace robotweax::srt::compat::hybrid
