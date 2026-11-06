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

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_PEERAUTH
// Owns a provider-native P-256 private key; destruction releases/erases it.
// No raw private-key export. A handle is session-owned, not used concurrently.
// Points are fixed-width big-endian X || Y; signatures are r || s. All failed
// output-producing operations must erase the complete output. Input and output
// spans must not overlap. agree() validates the received point before ECDH.
class P256Key {
public:
    virtual ~P256Key() = default;
    [[nodiscard]] virtual Error public_key(
        std::span<std::byte, 64>) noexcept = 0;
    [[nodiscard]] virtual Error agree(std::span<const std::byte, 64> peer,
        std::span<std::byte, 32> secret) noexcept = 0;
    [[nodiscard]] virtual Error sign_sha256(std::span<const std::byte> message,
        std::span<std::byte, 64> signature) noexcept = 0;
};
#endif

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

    // Optional primitive for the explicitly enabled session-binding extension.
    // Existing custom providers fail closed until they implement it.
    [[nodiscard]] virtual Error hmac_sha256(std::span<const std::byte>,
        std::span<const std::byte>, std::span<std::byte, 32> output) noexcept
    {
        secure_erase(output);
        return Error::unsupported;
    }

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_PEERAUTH
    // Optional operations: providers without P-256 support fail closed.
    [[nodiscard]] virtual Error generate_p256_key(
        std::unique_ptr<P256Key>& key) noexcept
    {
        key.reset();
        return Error::unsupported;
    }
    [[nodiscard]] virtual Error import_p256_key(
        std::span<const std::byte, 32>, std::unique_ptr<P256Key>& key) noexcept
    {
        key.reset();
        return Error::unsupported;
    }
    [[nodiscard]] virtual Error validate_p256_public_key(
        std::span<const std::byte, 64>) noexcept
    {
        return Error::unsupported;
    }
    [[nodiscard]] virtual Error verify_p256_sha256(
        std::span<const std::byte, 64>, std::span<const std::byte>,
        std::span<const std::byte, 64>) noexcept
    {
        return Error::unsupported;
    }

    // Optional transcript primitive. Custom providers fail closed until supplied.
    [[nodiscard]] virtual Error sha256(
        std::span<const std::byte>, std::span<std::byte, 32> output) noexcept
    {
        secure_erase(output);
        return Error::unsupported;
    }
#endif

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
