// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Robotweax GmbH and contributors
#include "compat/random_identity.hpp"
#include "compat/process_owned.hpp"

#include "robotweax/srt/crypto_provider.hpp"
#include "robotweax/srt/sequence.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>

namespace robotweax::srt::compat {
namespace {

using Block = std::array<std::byte, aes_block_size>;

constexpr unsigned handle_half_bits = 15;
constexpr std::uint32_t handle_half_mask = (1U << handle_half_bits) - 1U;
static_assert(registry_handle_space == 1U << (2U * handle_half_bits));
// Eight Feistel rounds over an AES-based round function, as in the NIST FF1
// format-preserving construction (which uses ten over a similar split).
constexpr std::uint8_t handle_rounds = 8;

// Two AES-128 keys drawn once from the provider: one for a CTR keystream
// (initial sequence numbers), one for the handle permutation. Drawing the
// keys once keeps the provider's random source off runtime worker threads.
// Those threads live until process exit, and a per-thread random generator
// released that late is reported as leaked by LeakSanitizer.
class IdentityGenerator {
public:
    [[nodiscard]] static IdentityGenerator& instance() noexcept
    {
        // The provider is constructed first so that it outlives the ciphers.
        (void)default_crypto_provider();
        static ProcessOwned<IdentityGenerator> generator;
        return generator.get();
    }

    [[nodiscard]] bool prepare() noexcept
    {
        std::lock_guard lock(mutex_);
        return seed_locked();
    }

    [[nodiscard]] std::optional<std::uint32_t> next_word() noexcept
    {
        std::lock_guard lock(mutex_);
        if (!seed_locked()) {
            return std::nullopt;
        }
        if (available_ < sizeof(std::uint32_t) && !refill_locked()) {
            return std::nullopt;
        }
        const std::size_t offset = keystream_.size() - available_;
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < sizeof(value); ++index) {
            value = (value << 8U)
                | std::to_integer<std::uint32_t>(keystream_[offset + index]);
            keystream_[offset + index] = std::byte {0};
        }
        available_ -= sizeof(value);
        return value;
    }

    // Keyed permutation of [0, registry_handle_space) per handle space.
    [[nodiscard]] std::optional<std::uint32_t> permute(
        HandleSpace space, std::uint32_t value) noexcept
    {
        std::lock_guard lock(mutex_);
        if (!seed_locked()) {
            return std::nullopt;
        }
        std::uint32_t left = value >> handle_half_bits;
        std::uint32_t right = value & handle_half_mask;
        for (std::uint8_t round = 0; round < handle_rounds; ++round) {
            const auto mixed = round_function_locked(space, round, right);
            if (!mixed) {
                return std::nullopt;
            }
            const std::uint32_t next_right = left ^ *mixed;
            left = right;
            right = next_right;
        }
        return (left << handle_half_bits) | right;
    }

private:
    friend class ProcessOwned<IdentityGenerator>;

    static constexpr std::size_t keystream_blocks = 4;

    IdentityGenerator() = default;

    ~IdentityGenerator()
    {
        default_crypto_provider().secure_erase(keystream_);
        default_crypto_provider().secure_erase(counter_);
    }

    [[nodiscard]] bool seed_locked() noexcept
    {
        if (keystream_cipher_ != nullptr && permutation_cipher_ != nullptr) {
            return true;
        }
        auto& provider = default_crypto_provider();
        std::array<std::byte, 2 * aes_block_size> keys {};
        const auto keystream_key = std::span {keys}.first(aes_block_size);
        const auto permutation_key = std::span {keys}.last(aes_block_size);
        const bool seeded = provider.random_bytes(keys) == Error::none
            && provider.random_bytes(counter_) == Error::none
            && provider.make_aes_ctr_cipher(keystream_key, keystream_cipher_)
                == Error::none
            && provider.make_aes_ctr_cipher(
                   permutation_key, permutation_cipher_)
                == Error::none
            && keystream_cipher_ != nullptr && permutation_cipher_ != nullptr;
        provider.secure_erase(keys);
        if (!seeded) {
            keystream_cipher_.reset();
            permutation_cipher_.reset();
            provider.secure_erase(counter_);
        }
        available_ = 0;
        return seeded;
    }

    [[nodiscard]] bool refill_locked() noexcept
    {
        keystream_.fill(std::byte {0});
        if (keystream_cipher_->transform(counter_, keystream_, keystream_)
            != Error::none) {
            return false;
        }
        // Advance the 128-bit big-endian counter past the blocks just used.
        unsigned carry = keystream_blocks;
        for (std::size_t index = counter_.size(); index-- > 0 && carry != 0;) {
            const unsigned sum =
                std::to_integer<unsigned>(counter_[index]) + carry;
            counter_[index] = static_cast<std::byte>(sum & 0xffU);
            carry = sum >> 8U;
        }
        available_ = keystream_.size();
        return true;
    }

    // AES_k(space || round || value) truncated to one half. CTR over one zero
    // block with the encoded input as counter block yields exactly AES_k(x).
    [[nodiscard]] std::optional<std::uint32_t> round_function_locked(
        HandleSpace space, std::uint8_t round, std::uint32_t value) noexcept
    {
        Block input {};
        input[0] = static_cast<std::byte>(space);
        input[1] = static_cast<std::byte>(round);
        input[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
        input[3] = static_cast<std::byte>(value & 0xffU);
        Block output {};
        if (permutation_cipher_->transform(input, output, output)
            != Error::none) {
            return std::nullopt;
        }
        const std::uint32_t mixed =
            (std::to_integer<std::uint32_t>(output[0]) << 8U)
            | std::to_integer<std::uint32_t>(output[1]);
        default_crypto_provider().secure_erase(output);
        return mixed & handle_half_mask;
    }

    std::mutex mutex_;
    std::unique_ptr<PayloadCipher> keystream_cipher_;
    std::unique_ptr<PayloadCipher> permutation_cipher_;
    Block counter_ {};
    std::array<std::byte, keystream_blocks * aes_block_size> keystream_ {};
    std::size_t available_ = 0;
};

} // namespace

bool prepare_random_identity() noexcept
{
    return IdentityGenerator::instance().prepare();
}

SRTSOCKET next_registry_handle(HandleSpace space, std::uint32_t& index) noexcept
{
    // Position zero of the permutation maps somewhere; the one position that
    // maps to zero is skipped, leaving registry_handle_space - 1 handles.
    while (index < registry_handle_space) {
        const auto handle = IdentityGenerator::instance().permute(space, index);
        if (!handle) {
            return SRT_INVALID_SOCK;
        }
        ++index;
        if (*handle != 0U) {
            return static_cast<SRTSOCKET>(*handle);
        }
    }
    return SRT_INVALID_SOCK;
}

std::optional<std::uint32_t> random_initial_sequence() noexcept
{
    const auto word = IdentityGenerator::instance().next_word();
    if (!word) {
        return std::nullopt;
    }
    // The reference implementation treats an ISN of 0x7FFFFFFF as invalid
    // and rejects the handshake as rogue; never generate it.
    return *word % SequenceNumber::mask;
}

} // namespace robotweax::srt::compat
