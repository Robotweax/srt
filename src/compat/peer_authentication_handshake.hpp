// SPDX-License-Identifier: MIT
#pragma once
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_PEERAUTH
#include "peer_authentication_admission.hpp"
#include "robotweax/srt/handshake_datagram.hpp"

namespace robotweax::srt::compat {
struct ListenerHandshakeAdmission;

// Connection-owned, serialized packet adapter for the experimental profile.
// The owning socket supplies address routing, cookie secrets/admission budgets,
// callback-selected options, timers and I/O. Invoke off-shard: some transitions
// perform public-key crypto/PBKDF. Negotiated transport state and crypto remain
// inaccessible until authenticated runtime key confirmation completes.
// The I/O owner must retain bounded early DATA for runtime handoff; this
// control-only adapter does not consume or queue application packets.
class PeerAuthenticationHandshake {
public:
    using Time = peer_authentication::Admission::Time;
    struct Datagram {
        std::array<std::byte,
            packet_header_size + peer_authentication::maximum_payload_size>
            bytes {};
        std::size_t size = 0;
        [[nodiscard]] std::span<const std::byte> view() const noexcept
        {
            return std::span {bytes}.first(size);
        }
    };
    struct Result {
        Error error = Error::none;
        std::array<Datagram, 2> outgoing {};
        std::size_t size = 0;
        bool ready = false;
        bool failed = false;
    };
    PeerAuthenticationHandshake(HandshakeMachine::Configuration configuration,
        const SocketOptions& options,
        CryptoProvider& provider = default_crypto_provider()) noexcept;
    ~PeerAuthenticationHandshake();
    PeerAuthenticationHandshake(const PeerAuthenticationHandshake&) = delete;
    PeerAuthenticationHandshake& operator=(
        const PeerAuthenticationHandshake&) = delete;
    [[nodiscard]] Result start(Time now, Time deadline) noexcept;
    // Starts directly from the existing stateless listener admission result,
    // after its callback and work-budget checks. The configured cookie callback
    // must return that admission's validated cookie.
    [[nodiscard]] Result start_admitted_listener(
        const ListenerHandshakeAdmission& admitted, Time now,
        Time deadline) noexcept;
    [[nodiscard]] Result receive(
        std::span<const std::byte> datagram, Time now) noexcept;
    [[nodiscard]] Result retry(Time now) noexcept;
    void abort() noexcept;
    [[nodiscard]] bool ready(Time now) noexcept;
    [[nodiscard]] std::shared_ptr<CryptoSession> release_crypto(
        Time now) noexcept;
    [[nodiscard]] std::span<const std::byte> peer_public_key(Time now) noexcept;
    [[nodiscard]] const HandshakeMachine* transport(Time now) noexcept;
    [[nodiscard]] std::uint32_t peer_socket_id() const noexcept
    {
        return peer_socket_id_;
    }

private:
    enum class State {
        fresh,
        discovery,
        proofs,
        keys,
        ready,
        released,
        failed
    };
    [[nodiscard]] static HandshakeMachine::Configuration
    transport_configuration(HandshakeMachine::Configuration configuration,
        const SocketOptions& options) noexcept;
    [[nodiscard]] bool active(Time now) noexcept;
    [[nodiscard]] Result finish(Result result) noexcept;
    void fail(Result& result, Error error) noexcept;
    void clear_credentials() noexcept;
    [[nodiscard]] bool configure_admission(Result& result, Time now) noexcept;
    [[nodiscard]] PacketTimestamp timestamp(Time now) const noexcept;
    void emit_action(
        const HandshakeAction& action, Result& result, Time now) noexcept;
    void emit_proof(HandshakeAction action,
        const peer_authentication::Record& record, Result& result,
        Time now) noexcept;
    void emit_cached(peer_authentication::Message message, Result& result,
        Time now) noexcept;
    void emit_key_request(Result& result, Time now) noexcept;
    void emit_key(bool response, std::span<const std::byte> payload,
        Result& result, Time now) noexcept;
    [[nodiscard]] bool transport_result(
        const HandshakeActions& actions, Result& result) noexcept;
    HandshakeMachine::Configuration configuration_;
    SocketOptions options_;
    HandshakeMachine transport_;
    peer_authentication::Admission admission_;
    bool caller_;
    State state_ = State::fresh;
    Time origin_ {}, deadline_ {};
    std::uint32_t peer_socket_id_ = 0;
    unsigned retries_ = 0;
    std::uint8_t key_length_ = 0;
    Handshake offer_header_ {};
};
} // namespace robotweax::srt::compat
#endif
