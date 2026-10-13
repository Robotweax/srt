#include "robotweax/srt/session_authentication.hpp"

#include "robotweax/srt/codec.hpp"

#include <algorithm>
#include <limits>

namespace robotweax::srt {
namespace {
bool zero(std::span<const std::byte> bytes) noexcept
{
    std::byte value {};
    for (const auto byte : bytes)
        value |= byte;
    return value == std::byte {};
}
bool equal(
    std::span<const std::byte> left, std::span<const std::byte> right) noexcept
{
    if (left.size() != right.size())
        return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i)
        difference |= std::to_integer<unsigned>(left[i] ^ right[i]);
    return difference == 0;
}
void put64(std::span<std::byte, 8> out, std::uint64_t value) noexcept
{
    for (unsigned i = 0; i < 8; ++i)
        out[i] = std::byte(value >> (56U - 8U * i));
}
std::uint64_t get64(std::span<const std::byte, 8> in) noexcept
{
    std::uint64_t value = 0;
    for (const auto byte : in)
        value = (value << 8U) | std::to_integer<unsigned>(byte);
    return value;
}
constexpr std::array<std::byte, 16> domain_label {std::byte {'R'},
    std::byte {'W'}, std::byte {'S'}, std::byte {'R'}, std::byte {'T'},
    std::byte {'-'}, std::byte {'S'}, std::byte {'E'}, std::byte {'S'},
    std::byte {'S'}, std::byte {'I'}, std::byte {'O'}, std::byte {'N'},
    std::byte {'-'}, std::byte {'2'}, std::byte {0}};
}

bool SessionAuthenticationParameters::is_offer() const noexcept
{
    return !zero(caller_nonce) && zero(listener_nonce) && zero(proof);
}
bool SessionAuthenticationParameters::is_proof() const noexcept
{
    return !zero(caller_nonce) && !zero(listener_nonce) && !zero(proof);
}
bool SessionAuthenticationParameters::matches(
    const SessionAuthenticationParameters& peer) const noexcept
{
    return version == peer.version
        && (static_cast<unsigned>(equal(caller_nonce, peer.caller_nonce))
               & static_cast<unsigned>(
                   equal(listener_nonce, peer.listener_nonce))
               & static_cast<unsigned>(equal(proof, peer.proof)))
        != 0;
}
SessionAuthentication::~SessionAuthentication()
{
    provider_.secure_erase(key_);
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
    provider_.secure_erase(caller_control_key_);
    provider_.secure_erase(listener_control_key_);
#endif
    provider_.secure_erase(last_sent_);
    provider_.secure_erase(last_received_);
}
Error SessionAuthentication::start(bool caller, std::uint8_t version) noexcept
{
    if (version != session_authentication_version
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
        && !is_acp1_session_version(version)
#endif
    )
        return Error::invalid_state;
    if (started_)
        return Error::invalid_state;
    caller_ = caller;
    parameters_.version = version;
    auto& nonce =
        caller ? parameters_.caller_nonce : parameters_.listener_nonce;
    const auto result = provider_.random_bytes(nonce);
    if (result != Error::none || zero(nonce)) {
        provider_.secure_erase(nonce);
        return Error::cryptographic_failure;
    }
    started_ = true;
    return Error::none;
}
Error SessionAuthentication::establish(std::span<const std::byte> passphrase,
    const SessionAuthenticationParameters& peer, std::uint32_t local_socket_id,
    std::uint32_t peer_socket_id, std::uint32_t local_cookie,
    std::uint32_t peer_cookie) noexcept
{
    if (!started_ || ready_ || passphrase.empty() || !local_socket_id
        || !peer_socket_id)
        return Error::invalid_state;
    if (parameters_.version == acp1_rendezvous_session_version
            ? (!local_cookie || !peer_cookie || local_cookie == peer_cookie)
            : (local_cookie != 0 || peer_cookie != 0))
        return Error::invalid_key_material;
    if (peer.version != parameters_.version)
        return Error::invalid_key_material;
    if (caller_) {
        if (!equal(peer.caller_nonce, parameters_.caller_nonce)
            || zero(peer.listener_nonce))
            return Error::invalid_key_material;
        parameters_.listener_nonce = peer.listener_nonce;
    } else {
        if (!peer.is_offer())
            return Error::invalid_key_material;
        parameters_.caller_nonce = peer.caller_nonce;
    }
    std::array<std::byte, 112> salt {};
    std::copy(domain_label.begin(), domain_label.end(), salt.begin());
    salt[14] =
        std::byte {static_cast<unsigned char>('0' + parameters_.version)};
    std::copy(parameters_.caller_nonce.begin(), parameters_.caller_nonce.end(),
        salt.begin() + 16);
    std::copy(parameters_.listener_nonce.begin(),
        parameters_.listener_nonce.end(), salt.begin() + 48);
    put64(std::span {salt}.subspan<80, 8>(),
        caller_ ? local_socket_id : peer_socket_id);
    put64(std::span {salt}.subspan<88, 8>(),
        caller_ ? peer_socket_id : local_socket_id);
    const bool rendezvous =
        parameters_.version == acp1_rendezvous_session_version;
    if (rendezvous) {
        put64(std::span {salt}.subspan<96, 8>(),
            caller_ ? local_cookie : peer_cookie);
        put64(std::span {salt}.subspan<104, 8>(),
            caller_ ? peer_cookie : local_cookie);
    }
    const auto result = provider_.pbkdf2_hmac_sha1(passphrase,
        std::span {salt}.first(rendezvous ? 112U : 96U), 2048, key_);
    if (result != Error::none) {
        provider_.secure_erase(key_);
        return result;
    }
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
    if (is_acp1_session_version(parameters_.version)) {
        // RFC 5869 single-block Expand, with the session root as PRK.
        std::array<std::byte, 18> info {};
        constexpr char label[] = "RWSRT-ACP1-1";
        for (std::size_t i = 0; i < sizeof(label) - 1; ++i)
            info[i] = std::byte {static_cast<unsigned char>(label[i])};
        info[17] = std::byte {1};
        for (const bool caller : {true, false}) {
            info[16] = std::byte {static_cast<unsigned char>(caller ? 1 : 2)};
            const auto derived = provider_.hmac_sha256(key_, info,
                caller ? caller_control_key_ : listener_control_key_);
            if (derived != Error::none) {
                provider_.secure_erase(key_);
                provider_.secure_erase(caller_control_key_);
                provider_.secure_erase(listener_control_key_);
                return derived;
            }
        }
    }
#endif
    ready_ = true;
    return Error::none;
}
Error SessionAuthentication::mac(std::uint8_t domain, bool caller,
    std::uint64_t counter, std::span<const std::byte> input,
    std::span<std::byte, 32> output) noexcept
{
    if (!ready_ || input.size() > last_sent_.size())
        return Error::invalid_state;
    std::array<std::byte, 156> bytes {};
    std::copy(domain_label.begin(), domain_label.end(), bytes.begin());
    bytes[14] =
        std::byte {static_cast<unsigned char>('0' + parameters_.version)};
    bytes[16] = std::byte {domain};
    bytes[17] = static_cast<std::byte>(caller ? 1U : 2U);
    put64(std::span {bytes}.subspan<18, 8>(), counter);
    bytes[26] = std::byte(input.size() >> 8U);
    bytes[27] = std::byte(input.size());
    std::copy(input.begin(), input.end(), bytes.begin() + 28);
    return provider_.hmac_sha256(
        key_, std::span {bytes}.first(28 + input.size()), output);
}
Error SessionAuthentication::handshake_proof(bool caller,
    std::span<const std::byte> initial_material,
    SessionAuthenticationParameters& output) noexcept
{
    output = parameters_;
    return mac(1, caller, 0, initial_material, output.proof);
}
bool SessionAuthentication::verify_handshake(bool caller,
    std::span<const std::byte> initial_material,
    const SessionAuthenticationParameters& peer) noexcept
{
    SessionAuthenticationParameters expected;
    return handshake_proof(caller, initial_material, expected) == Error::none
        && expected.matches(peer);
}
Error SessionAuthentication::seal(bool response,
    std::span<const std::byte> material, std::span<std::byte> output,
    std::size_t& written) noexcept
{
    written = 0;
    if (!ready_ || material.empty() || material.size() > last_sent_.size()
        || output.size() < material.size() + authenticated_key_overhead)
        return Error::invalid_state;
    auto counter = response ? received_counter_ : sent_counter_;
    const bool changed = !response
        && !equal(material, std::span {last_sent_}.first(last_sent_size_));
    if (changed) {
        if (counter == std::numeric_limits<std::uint64_t>::max())
            return Error::invalid_state;
        ++counter;
    }
    if (counter == 0)
        return Error::invalid_state;
    std::array<std::byte, 32> tag {};
    const auto result = mac(response ? 3 : 2, caller_, counter, material, tag);
    if (result != Error::none)
        return result;
    put64(output.first<8>(), counter);
    std::copy(material.begin(), material.end(), output.begin() + 8);
    std::copy(
        tag.begin(), tag.end(), output.subspan(8 + material.size()).begin());
    if (changed) {
        std::copy(material.begin(), material.end(), last_sent_.begin());
        last_sent_size_ = material.size();
        sent_counter_ = counter;
    }
    written = material.size() + authenticated_key_overhead;
    return Error::none;
}
bool SessionAuthentication::open(bool response,
    std::span<const std::byte> input,
    std::span<const std::byte>& material) noexcept
{
    material = {};
    if (!ready_ || input.size() <= authenticated_key_overhead
        || input.size() > last_received_.size() + authenticated_key_overhead)
        return false;
    const auto counter = get64(input.first<8>());
    const auto payload =
        input.subspan(8, input.size() - authenticated_key_overhead);
    if (counter == 0
        || (response ? counter != sent_counter_ : counter < received_counter_))
        return false;
    std::array<std::byte, 32> tag {};
    if (mac(response ? 3 : 2, !caller_, counter, payload, tag) != Error::none
        || !equal(tag, input.last<32>()))
        return false;
    if (!response) {
        if (counter == received_counter_
            && !equal(
                payload, std::span {last_received_}.first(last_received_size_)))
            return false;
        std::copy(payload.begin(), payload.end(), last_received_.begin());
        last_received_size_ = payload.size();
        received_counter_ = counter;
    }
    material = payload;
    return true;
}
Error SessionAuthentication::fec_mac(bool caller, std::uint64_t counter,
    const DataHeader& header, std::span<const std::byte> parity,
    std::span<std::byte, 32> output) noexcept
{
    if (!ready_ || parity.empty()
        || parity.size()
            > maximum_data_payload_size - authenticated_fec_overhead
        || header.message_number != 0U)
        return Error::invalid_state;
    std::array<std::byte, 28 + packet_header_size + maximum_data_payload_size>
        bytes {};
    std::copy(domain_label.begin(), domain_label.end(), bytes.begin());
    bytes[14] =
        std::byte {static_cast<unsigned char>('0' + parameters_.version)};
    bytes[16] = std::byte {4};
    bytes[17] = static_cast<std::byte>(caller ? 1U : 2U);
    put64(std::span {bytes}.subspan<18, 8>(), counter);
    bytes[26] = std::byte(parity.size() >> 8U);
    bytes[27] = std::byte(parity.size());
    MutablePacketView packet;
    packet.kind = PacketKind::data;
    packet.data = header;
    const auto encoded = encode_packet(
        packet, std::span {bytes}.subspan(28, packet_header_size));
    if (!encoded)
        return encoded.error;
    std::copy(
        parity.begin(), parity.end(), bytes.begin() + 28 + packet_header_size);
    return provider_.hmac_sha256(key_,
        std::span {bytes}.first(28 + packet_header_size + parity.size()),
        output);
}

Error SessionAuthentication::seal_fec(const DataHeader& header,
    std::span<const std::byte> parity, std::span<std::byte> output,
    std::size_t& written) noexcept
{
    written = 0;
    if (fec_sent_counter_ == std::numeric_limits<std::uint64_t>::max()
        || output.size() < parity.size() + authenticated_fec_overhead)
        return Error::invalid_state;
    const auto counter = fec_sent_counter_ + 1;
    std::array<std::byte, 32> tag {};
    const auto result = fec_mac(caller_, counter, header, parity, tag);
    if (result != Error::none)
        return result;
    put64(output.first<8>(), counter);
    std::copy(parity.begin(), parity.end(), output.begin() + 8);
    std::copy(
        tag.begin(), tag.end(), output.subspan(8 + parity.size()).begin());
    fec_sent_counter_ = counter;
    written = parity.size() + authenticated_fec_overhead;
    return Error::none;
}

bool SessionAuthentication::open_fec(const DataHeader& header,
    std::span<const std::byte> input,
    std::span<const std::byte>& parity) noexcept
{
    parity = {};
    if (input.size() <= authenticated_fec_overhead
        || input.size() > maximum_data_payload_size)
        return false;
    const auto counter = get64(input.first<8>());
    if (counter == 0)
        return false;
    const auto age =
        counter <= fec_received_counter_ ? fec_received_counter_ - counter : 0;
    if (counter <= fec_received_counter_
        && (age >= 64 || (fec_received_bitmap_ & (std::uint64_t {1} << age))))
        return false;
    const auto body =
        input.subspan(8, input.size() - authenticated_fec_overhead);
    std::array<std::byte, 32> tag {};
    if (fec_mac(!caller_, counter, header, body, tag) != Error::none
        || !equal(tag, input.last<32>()))
        return false;
    // Only a valid MAC may move the receive window.
    if (counter > fec_received_counter_) {
        const auto advance = counter - fec_received_counter_;
        fec_received_bitmap_ =
            advance >= 64 ? 1U : (fec_received_bitmap_ << advance) | 1U;
        fec_received_counter_ = counter;
    } else {
        fec_received_bitmap_ |= std::uint64_t {1} << age;
    }
    parity = body;
    return true;
}
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
namespace {
bool acp1_control(const PacketView& packet) noexcept
{
    if (packet.kind != PacketKind::control
        || !has_valid_control_payload_shape(packet.payload))
        return false;
    if (packet.control.type == ControlType::handshake)
        return false;
    if (packet.control.type == ControlType::user_defined)
        return packet.control.subtype == authenticated_key_request_subtype
            || packet.control.subtype == authenticated_key_response_subtype;
    return packet.control.subtype == 0;
}
}
Error SessionAuthentication::control_mac(bool caller,
    std::span<const std::byte> wire, std::span<std::byte, 32> tag) noexcept
{
    if (!ready_ || !is_acp1_session_version(parameters_.version)
        || wire.size() > 1500U - 32U)
        return Error::invalid_state;
    // Keys already separate session, profile and direction. This domain
    // additionally separates envelope tags from key expansion.
    std::array<std::byte, 16 + 1500> input {};
    constexpr char label[] = "RWSRT-ACP1-MAC";
    for (std::size_t i = 0; i < sizeof(label) - 1; ++i)
        input[i] = std::byte {static_cast<unsigned char>(label[i])};
    std::copy(wire.begin(), wire.end(), input.begin() + 16);
    return provider_.hmac_sha256(
        caller ? caller_control_key_ : listener_control_key_,
        std::span {input}.first(16 + wire.size()), tag);
}
Error SessionAuthentication::seal_control(std::span<const std::byte> packet,
    std::span<std::byte> output, std::size_t& written) noexcept
{
    written = 0;
    if (!ready_ || !is_acp1_session_version(parameters_.version)
        || packet.size() > 1500U - authenticated_control_overhead
        || output.size() < packet.size() + authenticated_control_overhead
        || control_sent_counter_ == std::numeric_limits<std::uint64_t>::max())
        return Error::invalid_state;
    const auto decoded = decode_packet(packet);
    if (!decoded || !acp1_control(decoded.packet))
        return Error::invalid_control_type;
    const auto counter = control_sent_counter_ + 1;
    std::array<std::byte, 1500> wire {};
    MutablePacketView outer;
    outer.kind = PacketKind::control;
    outer.control = decoded.packet.control;
    outer.control.type = ControlType::user_defined;
    outer.control.subtype = authenticated_control_subtype;
    outer.control.type_specific = 0;
    // Encode only the outer header; build the authenticated payload below.
    if (!encode_packet(outer, wire))
        return Error::invalid_state;
    wire[19] = std::byte {1}; // u32 envelope version, network order
    put64(std::span {wire}.subspan<20, 8>(), counter);
    std::copy(packet.begin(), packet.end(),
        wire.begin() + authenticated_control_inner_offset);
    const auto tag_offset = authenticated_control_inner_offset + packet.size();
    const auto result = control_mac(caller_, std::span {wire}.first(tag_offset),
        std::span {wire}.subspan(tag_offset, 32).first<32>());
    if (result != Error::none)
        return result;
    written = packet.size() + authenticated_control_overhead;
    std::copy_n(wire.begin(), written, output.begin());
    control_sent_counter_ = counter;
    return Error::none;
}
bool SessionAuthentication::open_control(
    const PacketView& envelope, PacketView& packet) noexcept
{
    packet = {};
    if (!ready_ || !is_acp1_session_version(parameters_.version)
        || envelope.kind != PacketKind::control
        || envelope.control.type != ControlType::user_defined
        || envelope.control.subtype != authenticated_control_subtype
        || envelope.control.type_specific != 0
        || envelope.payload.size() < 12U + packet_header_size + 4U + 32U
        || envelope.payload.size() > 1500U - packet_header_size
        || envelope.payload[0] != std::byte {0}
        || envelope.payload[1] != std::byte {0}
        || envelope.payload[2] != std::byte {0}
        || envelope.payload[3] != std::byte {1})
        return false;
    const auto counter = get64(envelope.payload.subspan<4, 8>());
    if (counter == 0)
        return false;
    if (counter <= control_received_counter_) {
        const auto age = control_received_counter_ - counter;
        if (age >= 64U
            || (control_received_bitmap_ & (std::uint64_t {1} << age)) != 0)
            return false;
    }
    const auto inner_bytes =
        envelope.payload.subspan(12, envelope.payload.size() - 44);
    const auto decoded = decode_packet(inner_bytes);
    if (!decoded || !acp1_control(decoded.packet)
        || decoded.packet.control.timestamp != envelope.control.timestamp
        || decoded.packet.control.destination_socket_id
            != envelope.control.destination_socket_id)
        return false;
    std::array<std::byte, 1500> wire {};
    const auto encoded = encode_packet(
        {.kind = PacketKind::control,
            .control = envelope.control,
            .payload = envelope.payload.first(envelope.payload.size() - 32)},
        wire);
    std::array<std::byte, 32> expected {};
    if (!encoded
        || control_mac(!caller_, std::span {wire}.first(encoded.bytes_written),
               expected)
            != Error::none
        || !equal(expected, envelope.payload.last<32>()))
        return false;
    // Commit replay state only after authentication. A forged large counter
    // cannot move the window and suppress subsequent legitimate controls.
    if (counter > control_received_counter_) {
        const auto distance = counter - control_received_counter_;
        control_received_bitmap_ =
            distance >= 64U ? 1U : (control_received_bitmap_ << distance) | 1U;
        control_received_counter_ = counter;
    } else {
        control_received_bitmap_ |= std::uint64_t {1}
            << (control_received_counter_ - counter);
    }
    packet = decoded.packet;
    return true;
}
#endif

} // namespace robotweax::srt
