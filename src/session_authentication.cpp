#include "robotweax/srt/session_authentication.hpp"

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
    std::byte {'-'}, std::byte {'1'}, std::byte {0}};
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
    return (static_cast<unsigned>(equal(caller_nonce, peer.caller_nonce))
               & static_cast<unsigned>(
                   equal(listener_nonce, peer.listener_nonce))
               & static_cast<unsigned>(equal(proof, peer.proof)))
        != 0;
}
SessionAuthentication::~SessionAuthentication()
{
    provider_.secure_erase(key_);
    provider_.secure_erase(last_sent_);
    provider_.secure_erase(last_received_);
}
Error SessionAuthentication::start(bool caller) noexcept
{
    if (started_)
        return Error::invalid_state;
    caller_ = caller;
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
    std::uint32_t peer_socket_id) noexcept
{
    if (!started_ || ready_ || passphrase.empty() || !local_socket_id
        || !peer_socket_id)
        return Error::invalid_state;
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
    std::array<std::byte, 96> salt {};
    std::copy(domain_label.begin(), domain_label.end(), salt.begin());
    std::copy(parameters_.caller_nonce.begin(), parameters_.caller_nonce.end(),
        salt.begin() + 16);
    std::copy(parameters_.listener_nonce.begin(),
        parameters_.listener_nonce.end(), salt.begin() + 48);
    put64(std::span {salt}.subspan<80, 8>(),
        caller_ ? local_socket_id : peer_socket_id);
    put64(std::span {salt}.subspan<88, 8>(),
        caller_ ? peer_socket_id : local_socket_id);
    const auto result =
        provider_.pbkdf2_hmac_sha1(passphrase, salt, 2048, key_);
    ready_ = result == Error::none;
    if (!ready_)
        provider_.secure_erase(key_);
    return result;
}
Error SessionAuthentication::mac(std::uint8_t domain, bool caller,
    std::uint64_t counter, std::span<const std::byte> input,
    std::span<std::byte, 32> output) noexcept
{
    if (!ready_ || input.size() > last_sent_.size())
        return Error::invalid_state;
    std::array<std::byte, 156> bytes {};
    std::copy(domain_label.begin(), domain_label.end(), bytes.begin());
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
} // namespace robotweax::srt
