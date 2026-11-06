// SPDX-License-Identifier: MIT
#include "peer_authentication_admission.hpp"

#include <algorithm>

namespace robotweax::srt::peer_authentication {
Admission::~Admission()
{
    abort();
}

void Admission::erase_passphrase() noexcept
{
    provider_.secure_erase(std::as_writable_bytes(std::span {passphrase_}));
    passphrase_size_ = 0;
    configuration_.passphrase = {};
}
void Admission::abort() noexcept
{
    exchange_.abort();
    crypto_.reset();
    erase_passphrase();
    peer_.fill(std::byte {});
    for (auto& flight : flights_)
        flight.size = 0;
    state_ = State::failed;
}
bool Admission::expire(Time now) noexcept
{
    if ((state_ == State::proofs || state_ == State::keys
            || state_ == State::ready)
        && now >= deadline_) {
        abort();
        return true;
    }
    return false;
}
bool Admission::active(Time now) noexcept
{
    (void)expire(now);
    return state_ == State::proofs || state_ == State::keys
        || state_ == State::ready;
}
Error Admission::configure(
    const SocketOptions& options, Time deadline, Time now) noexcept
{
    if (state_ != State::fresh)
        return Error::invalid_state;
    if (deadline <= now || options.passphrase().size() > passphrase_.size()) {
        abort();
        return Error::invalid_state;
    }
    const auto error = options.configure_peer_authentication(exchange_);
    if (error != Error::none) {
        abort();
        return error;
    }
    configuration_ = options.crypto_configuration();
    std::copy(options.passphrase().begin(), options.passphrase().end(),
        passphrase_.begin());
    passphrase_size_ = options.passphrase().size();
    configuration_.passphrase = {passphrase_.data(), passphrase_size_};
    deadline_ = deadline;
    state_ = State::proofs;
    return Error::none;
}
Error Admission::proof_result(Error error) noexcept
{
    if (exchange_.failed())
        abort();
    return error;
}
Error Admission::prepare_offer(
    std::uint8_t length, Record& output, Time now) noexcept
{
    output = {};
    if (!active(now) || state_ != State::proofs)
        return Error::invalid_state;
    return proof_result(exchange_.prepare_offer(length, output));
}
Error Admission::bind_offer(
    std::span<const std::byte> payload, Time now) noexcept
{
    if (!active(now) || state_ != State::proofs)
        return Error::invalid_state;
    return proof_result(exchange_.bind_offer(payload));
}
Error Admission::prepare_response(std::span<const std::byte> offer,
    std::uint8_t length, Record& output, Time now) noexcept
{
    output = {};
    if (!active(now) || state_ != State::proofs)
        return Error::invalid_state;
    return proof_result(exchange_.prepare_response(offer, length, output));
}
Error Admission::seal_response(std::span<std::byte> payload, Time now) noexcept
{
    if (!active(now) || state_ != State::proofs)
        return Error::invalid_state;
    return proof_result(exchange_.seal_response(payload));
}
Error Admission::prepare_confirmation(
    std::span<const std::byte> response, Record& output, Time now) noexcept
{
    output = {};
    if (!active(now) || state_ != State::proofs)
        return Error::invalid_state;
    return proof_result(exchange_.prepare_confirmation(response, output));
}
Error Admission::install_keys() noexcept
{
    try {
        crypto_ = std::make_shared<CryptoSession>(configuration_, provider_);
    } catch (...) {
        abort();
        return Error::cryptographic_failure;
    }
    const auto error = crypto_->start_peer_authentication(exchange_);
    if (error != Error::none) {
        abort();
        return error;
    }
    // Cache only public wire bytes/identity, then erase the duplicate key
    // schedule and passphrase. CryptoSession now owns the sole runtime keys.
    for (unsigned i = 0; i < flights_.size(); ++i) {
        const auto payload = exchange_.cached(static_cast<Message>(i + 1));
        std::copy(payload.begin(), payload.end(), flights_[i].bytes.begin());
        flights_[i].size = payload.size();
    }
    const auto peer = exchange_.peer_public_key();
    std::copy(peer.begin(), peer.end(), peer_.begin());
    exchange_.abort();
    erase_passphrase();
    state_ = State::keys;
    return Error::none;
}
Error Admission::seal_confirmation(
    std::span<std::byte> payload, Time now) noexcept
{
    if (!active(now) || state_ != State::proofs)
        return Error::invalid_state;
    const auto error = proof_result(exchange_.seal_confirmation(payload,
        std::as_bytes(std::span {passphrase_}.first(passphrase_size_))));
    return error == Error::none ? install_keys() : error;
}
Error Admission::accept_confirmation(
    std::span<const std::byte> payload, Time now) noexcept
{
    if (!active(now) || caller_)
        return Error::invalid_state;
    if (state_ != State::proofs) {
        const auto expected = cached(Message::confirmation, now);
        return std::equal(payload.begin(), payload.end(), expected.begin(),
                   expected.end())
            ? Error::none
            : Error::invalid_state;
    }
    const auto error = proof_result(exchange_.accept_confirmation(payload,
        std::as_bytes(std::span {passphrase_}.first(passphrase_size_))));
    return error == Error::none ? install_keys() : error;
}
std::span<const std::byte> Admission::cached(Message message, Time now) noexcept
{
    if (state_ != State::released && !active(now))
        return {};
    if (state_ == State::proofs)
        return exchange_.cached(message);
    const auto index = static_cast<unsigned>(message);
    if (index < 1 || index > flights_.size())
        return {};
    return flights_[index - 1].view();
}
Error Admission::key_request(
    std::span<std::byte> output, std::size_t& written, Time now) noexcept
{
    written = 0;
    if (!active(now) || !crypto_)
        return Error::invalid_state;
    const auto material = crypto_->pending_key_material();
    if (material.empty())
        return Error::none;
    if (output.size() < material.size() + authenticated_key_overhead)
        return Error::buffer_too_small;
    const auto error = crypto_->session_authentication()->seal(
        false, material, output, written);
    if (error != Error::none)
        abort();
    return error;
}
void Admission::update_ready() noexcept
{
    if (crypto_->ready_to_send_data()
        && crypto_->receiver_state() == CryptoState::secured)
        state_ = State::ready;
}
Error Admission::accept_key_control(bool response,
    std::span<const std::byte> input, std::span<std::byte> output,
    std::size_t& written, Time now) noexcept
{
    written = 0;
    if (!active(now) || !crypto_)
        return Error::invalid_state;
    // A response echoes the request body, so its envelope has the same size.
    // Check capacity before allowing a valid request to advance replay state.
    if (!response && output.size() < input.size())
        return Error::buffer_too_small;
    std::span<const std::byte> material;
    if (!crypto_->session_authentication()->open(response, input, material))
        return Error::authentication_failure;
    auto error = response ? crypto_->acknowledge_key_material(material, false)
                          : crypto_->accept_key_material(material, false);
    if (error == Error::none && !response)
        error = crypto_->session_authentication()->seal(
            true, crypto_->key_material_response(), output, written);
    if (error != Error::none) {
        abort();
        return error;
    }
    update_ready();
    return Error::none;
}
bool Admission::ready_to_publish(Time now) noexcept
{
    return active(now) && state_ == State::ready;
}
std::span<const std::byte> Admission::peer_public_key(Time now) noexcept
{
    if (!ready_to_publish(now))
        return {};
    return peer_;
}
std::shared_ptr<CryptoSession> Admission::release_crypto(Time now) noexcept
{
    if (!ready_to_publish(now))
        return {};
    state_ = State::released;
    return std::move(crypto_);
}
} // namespace robotweax::srt::peer_authentication
