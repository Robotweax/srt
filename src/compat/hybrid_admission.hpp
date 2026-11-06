// SPDX-License-Identifier: MIT
#pragma once

#include "compat/hybrid_control.hpp"
#include "robotweax/srt/handshake.hpp"

namespace robotweax::srt::compat::hybrid {
inline constexpr std::size_t maximum_conclusion_payload = 1484;
inline constexpr std::size_t admission_prefix_size = 224;
inline constexpr std::size_t maximum_admission_size =
    admission_prefix_size + 2 * maximum_conclusion_payload;
using CredentialId = std::array<std::byte, 16>;

// A local authorization decision, not a wire assertion or a credential lookup.
// The owner must resolve the ID to a separate pairwise random 32-byte secret.
struct Authorization {
    CredentialId credential_id {};
    std::uint32_t caller_group = 0, listener_group = 0;
    StreamId stream_id;
    std::uint64_t generation = 0, expires_at_us = 0;
};
struct AdmissionTranscript {
    std::array<std::byte, maximum_admission_size> bytes {};
    std::size_t size = 0;
    [[nodiscard]] std::span<const std::byte> view() const noexcept
    {
        return std::span {bytes}.first(size);
    }
};
// Payloads are exact, owned-on-success snapshots of the final Caller and
// Listener CONCLUSIONs, excluding the outer 16-byte control header. They
// include every extension byte, including ordinary session-authentication/KM.
[[nodiscard]] bool admission_transcript(const Context&, const Authorization&,
    std::span<const std::byte> caller_payload,
    std::span<const std::byte> listener_payload, AdmissionTranscript&) noexcept;

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
// Setup-only, serialized owner. No I/O, credential discovery or grant issuance.
// Keep it alive until all Sessions borrowing authentication() are destroyed.
class Admission {
public:
    enum class State { empty, prepared, established, confirmed, closed };
    explicit Admission(CryptoProvider& provider, bool caller) noexcept;
    ~Admission();
    Admission(const Admission&) = delete;
    Admission& operator=(const Admission&) = delete;
    [[nodiscard]] bool prepare(const Context&, const Authorization&,
        std::span<const std::byte> caller_payload,
        std::span<const std::byte> listener_payload,
        std::uint64_t now_us) noexcept;
    [[nodiscard]] Error establish(const CredentialId&,
        std::span<const std::byte> pairwise_secret,
        const SessionAuthenticationParameters& peer,
        std::uint64_t now_us) noexcept;
    [[nodiscard]] Error proof(SessionAuthenticationParameters&) noexcept;
    // Final callback/authorization revalidation is supplied by the owner.
    // Failure is terminal, including changed/expired grants and bad proofs.
    [[nodiscard]] bool confirm(const SessionAuthenticationParameters&,
        const Authorization& current, std::uint64_t now_us) noexcept;
    // The runtime owner must call this on grant changes and expiry, serialized
    // with control processing. There is no timer or policy store in this class.
    [[nodiscard]] bool revalidate(
        const Authorization& current, std::uint64_t now_us) noexcept;
    [[nodiscard]] const SessionAuthenticationParameters&
    parameters() const noexcept;
    [[nodiscard]] SessionAuthentication* authentication() noexcept;
    [[nodiscard]] State state() const noexcept
    {
        return state_;
    }
    void close() noexcept;

private:
    [[nodiscard]] bool current(const Authorization&, std::uint64_t) noexcept;
    SessionAuthentication authentication_;
    AdmissionTranscript transcript_;
    Authorization authorization_;
    std::array<std::byte, context_size> context_ {};
    std::uint32_t caller_id_ = 0, listener_id_ = 0;
    std::uint64_t last_now_ = 0;
    bool caller_ = false;
    State state_ = State::empty;
};
#endif
} // namespace robotweax::srt::compat::hybrid
