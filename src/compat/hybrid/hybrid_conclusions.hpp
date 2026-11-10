// SPDX-License-Identifier: MIT
#pragma once
#include "compat/hybrid/hybrid_admission.hpp"
#include <memory>

namespace robotweax::srt::compat::hybrid {
// Exact ordinary HSREQ/HSRSP conclusions, not the compact authentication
// AGREEMENT. Publication belongs to the successful native setup owner.
class Conclusions {
public:
    [[nodiscard]] std::span<const std::byte> caller_payload() const noexcept;
    [[nodiscard]] std::span<const std::byte> listener_payload() const noexcept;
    [[nodiscard]] bool matches(bool caller, std::uint32_t local_socket,
        std::uint32_t peer_socket) const noexcept;
    [[nodiscard]] const MulticastDispatch& dispatch() const noexcept
    {
        return dispatch_;
    }
    [[nodiscard]] const MulticastSource& source() const noexcept
    {
        return source_;
    }

private:
    friend class ConclusionCapture;
    std::array<std::byte, maximum_conclusion_payload> caller_ {}, listener_ {};
    std::size_t caller_size_ = 0, listener_size_ = 0;
    std::uint32_t caller_socket_ = 0, listener_socket_ = 0;
    MulticastDispatch dispatch_;
    MulticastSource source_;
    bool valid_ = false;
};
// Setup-owned and serialized. Ignores induction/AGREEMENT/other profiles;
// observe only actual accepted input or fully submitted output datagrams.
class ConclusionCapture {
public:
    [[nodiscard]] bool observe(std::span<const std::byte> datagram) noexcept;
    void invalidate() noexcept
    {
        failed_ = true;
    }
    [[nodiscard]] std::shared_ptr<const Conclusions> freeze(bool caller,
        std::uint32_t local_socket, std::uint32_t peer_socket) const noexcept;

private:
    Conclusions captured_;
    bool failed_ = false;
};
} // namespace robotweax::srt::compat::hybrid
