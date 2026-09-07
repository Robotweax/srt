#pragma once

#include "robotweax/srt/error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace robotweax::srt {

inline constexpr std::size_t aes_block_size = 16;

// A prepared payload cipher owns the provider-specific AES key schedule.
// transform() supports in-place operation and is allocation-free.
class PayloadCipher {
public:
    virtual ~PayloadCipher() = default;

    [[nodiscard]] virtual Error transform(
        std::span<const std::byte, aes_block_size> initialization_vector,
        std::span<const std::byte> input,
        std::span<std::byte> output) noexcept = 0;
};

// A prepared authenticated payload cipher owns the provider-specific key
// schedule. seal() and open() accept exact in-place operation. A successful
// operation writes one output byte for every input byte; open() erases its
// complete destination before returning an authentication failure.
class AuthenticatedPayloadCipher {
public:
    virtual ~AuthenticatedPayloadCipher() = default;

    [[nodiscard]] virtual Error seal(
        std::span<const std::byte> initialization_vector,
        std::span<const std::byte> additional_authenticated_data,
        std::span<const std::byte> plaintext, std::span<std::byte> ciphertext,
        std::span<std::byte> authentication_tag) noexcept = 0;

    [[nodiscard]] virtual Error open(
        std::span<const std::byte> initialization_vector,
        std::span<const std::byte> additional_authenticated_data,
        std::span<const std::byte> ciphertext,
        std::span<const std::byte> authentication_tag,
        std::span<std::byte> plaintext) noexcept = 0;
};

// This is the only boundary between the SRT protocol implementation and a
// cryptographic library. Expensive key setup happens when a key is installed,
// not in the per-packet path.
class CryptoProvider {
public:
    virtual ~CryptoProvider() = default;

    [[nodiscard]] virtual Error random_bytes(
        std::span<std::byte> destination) noexcept = 0;

    [[nodiscard]] virtual Error pbkdf2_hmac_sha1(
        std::span<const std::byte> passphrase,
        std::span<const std::byte> salt,
        std::uint32_t iterations,
        std::span<std::byte> derived_key) noexcept = 0;

    [[nodiscard]] virtual Error wrap_key(
        std::span<const std::byte> key_encrypting_key,
        std::span<const std::byte> plaintext_keys,
        std::span<std::byte> destination,
        std::size_t& bytes_written) noexcept = 0;

    [[nodiscard]] virtual Error unwrap_key(
        std::span<const std::byte> key_encrypting_key,
        std::span<const std::byte> wrapped_keys,
        std::span<std::byte> destination,
        std::size_t& bytes_written) noexcept = 0;

    [[nodiscard]] virtual Error make_aes_ctr_cipher(
        std::span<const std::byte> key,
        std::unique_ptr<PayloadCipher>& cipher) noexcept = 0;

    // A provider that returns false must reset the output and return
    // Error::unsupported from make_aes_gcm_cipher().
    [[nodiscard]] virtual bool supports_aes_gcm() const noexcept = 0;

    [[nodiscard]] virtual Error make_aes_gcm_cipher(
        std::span<const std::byte> key,
        std::unique_ptr<AuthenticatedPayloadCipher>& cipher) noexcept = 0;

    virtual void secure_erase(std::span<std::byte> bytes) noexcept = 0;
};

// The returned provider is process-wide and safe to use for constructing
// independent CryptoSession instances. Prepared ciphers are session-owned.
[[nodiscard]] CryptoProvider& default_crypto_provider() noexcept;

} // namespace robotweax::srt
