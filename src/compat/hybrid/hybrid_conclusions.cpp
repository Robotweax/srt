// SPDX-License-Identifier: MIT
#include "compat/hybrid/hybrid_conclusions.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include <algorithm>

namespace robotweax::srt::compat::hybrid {
static_assert(sizeof(ConclusionCapture) < 4096);
std::span<const std::byte> Conclusions::caller_payload() const noexcept
{
    return std::span {caller_}.first(caller_size_);
}
std::span<const std::byte> Conclusions::listener_payload() const noexcept
{
    return std::span {listener_}.first(listener_size_);
}
bool Conclusions::matches(
    bool caller, std::uint32_t local, std::uint32_t peer) const noexcept
{
    return valid_ && local && peer && local != peer
        && local == (caller ? caller_socket_ : listener_socket_)
        && peer == (caller ? listener_socket_ : caller_socket_);
}
bool ConclusionCapture::observe(std::span<const std::byte> datagram) noexcept
{
    if (datagram.size() > packet_header_size + maximum_conclusion_payload)
        return false;
    const auto decoded = decode_handshake_datagram(datagram);
    if (!decoded || decoded.message.packet.version != handshake_version_5
        || decoded.message.packet.request != HandshakeRequest::conclusion
        || !decoded.message.has_handshake_extension
        || !decoded.message.has_multicast_source)
        return false;
    const auto& m = decoded.message;
    const bool caller =
        m.extension_type == HandshakeExtensionType::handshake_request;
    if (!caller
        && m.extension_type != HandshakeExtensionType::handshake_response)
        return false;
    const auto body = datagram.subspan(packet_header_size);
    auto& bytes = caller ? captured_.caller_ : captured_.listener_;
    auto& size = caller ? captured_.caller_size_ : captured_.listener_size_;
    bytes = {};
    std::copy(body.begin(), body.end(), bytes.begin());
    size = body.size();
    return true;
}
std::shared_ptr<const Conclusions> ConclusionCapture::freeze(
    bool caller, std::uint32_t local, std::uint32_t peer) const noexcept
{
    if (failed_ || !captured_.caller_size_ || !captured_.listener_size_)
        return {};
    // Decode from temporary outer headers solely for structural cross-checks;
    // the published payloads are copied unchanged from the original capture.
    const auto decode_body = [](std::span<const std::byte> body) {
        std::array<std::byte, 1500> packet {};
        const auto encoded = encode_packet(
            {.kind = PacketKind::control,
                .control = {.type = ControlType::handshake, .timestamp = {}},
                .payload = body},
            packet);
        return encoded
            ? decode_handshake_datagram(
                  std::span {packet}.first(encoded.bytes_written))
            : HandshakeDatagramDecodeResult {.error = Error::invalid_state};
    };
    const auto c = decode_body(captured_.caller_payload()),
               l = decode_body(captured_.listener_payload());
    if (!c || !l || !c.message.packet.socket_id || !l.message.packet.socket_id
        || c.message.packet.socket_id == l.message.packet.socket_id
        || c.message.multicast_source.kind != MulticastSourceKind::offer
        || !l.message.has_multicast_dispatch
        || l.message.multicast_source.kind != MulticastSourceKind::announcement
        || l.message.multicast_dispatch.group_id
            != l.message.group_membership.group_id
        || l.message.multicast_source.group_id
            != l.message.multicast_dispatch.group_id
        || l.message.multicast_source.endpoint.family
            != l.message.multicast_dispatch.multicast_group.family)
        return {};
    try {
        auto frozen = std::make_shared<Conclusions>(captured_);
        frozen->caller_socket_ = c.message.packet.socket_id;
        frozen->listener_socket_ = l.message.packet.socket_id;
        frozen->dispatch_ = l.message.multicast_dispatch;
        frozen->source_ = l.message.multicast_source;
        frozen->valid_ = true;
        return frozen->matches(caller, local, peer)
            ? frozen
            : std::shared_ptr<const Conclusions> {};
    } catch (...) {
        return {};
    }
}
} // namespace robotweax::srt::compat::hybrid
