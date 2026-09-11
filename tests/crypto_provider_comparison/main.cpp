// SPDX-License-Identifier: MIT
#include "robotweax/srt/crypto_provider.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace robotweax::srt {
CryptoProvider& openssl_test_provider() noexcept;
}
using namespace robotweax::srt;
void require(bool value)
{
    if (!value) {
        throw std::runtime_error("Provider mismatch");
    }
}
int main()
{
    try {
        auto& native = default_crypto_provider();
        auto& oracle = openssl_test_provider();
        for (const std::size_t key_size : {16U, 24U, 32U}) {
            std::vector<std::byte> key(key_size, std::byte {0x53});
            std::array<std::byte, 32> derived {}, expected {};
            require(native.pbkdf2_hmac_sha1(key, key, 2048, derived)
                == Error::none);
            require(oracle.pbkdf2_hmac_sha1(key, key, 2048, expected)
                == Error::none);
            require(derived == expected);
            for (const std::size_t size : {16U, 24U, 32U, 48U, 64U}) {
                std::vector<std::byte> plain(size, std::byte {0x17});
                std::vector<std::byte> a(size + 8), b(size + 8),
                    recovered(size);
                std::size_t na = 0, nb = 0;
                require(native.wrap_key(key, plain, a, na) == Error::none);
                require(oracle.wrap_key(key, plain, b, nb) == Error::none);
                require(a == b && na == nb);
                require(
                    native.unwrap_key(key, b, recovered, na) == Error::none);
                require(recovered == plain && na == size);
                b[0] ^= std::byte {1};
                require(native.unwrap_key(key, b, recovered, na) != Error::none
                    && na == 0);
            }
            std::unique_ptr<PayloadCipher> ctr_a, ctr_b;
            std::unique_ptr<AuthenticatedPayloadCipher> gcm_a, gcm_b;
            require(native.make_aes_ctr_cipher(key, ctr_a) == Error::none);
            require(oracle.make_aes_ctr_cipher(key, ctr_b) == Error::none);
            require(native.make_aes_gcm_cipher(key, gcm_a) == Error::none);
            require(oracle.make_aes_gcm_cipher(key, gcm_b) == Error::none);
            // Compare failure semantics, including whether caller storage is
            // erased or preserved. Success vectors alone cannot establish this.
            for (int fault = 0; fault < 4; ++fault) {
                std::array<std::byte, 12> nonce {};
                std::array<std::byte, 16> tag {};
                std::array<std::byte, 17> input {};
                std::array<std::byte, 17> a, b;
                a.fill(std::byte {0x5a});
                b = a;
                const auto iv = std::span<const std::byte>(nonce).first(
                    fault == 0 ? 11 : 12);
                const auto auth_tag =
                    std::span<const std::byte>(tag).first(fault == 1 ? 15 : 16);
                auto out_a =
                    std::span<std::byte>(a).first(fault == 2 ? 16 : 17);
                auto out_b =
                    std::span<std::byte>(b).first(fault == 2 ? 16 : 17);
                const auto result =
                    gcm_a->open(iv, key, input, auth_tag, out_a);
                require(result != Error::none);
                require(result == gcm_b->open(iv, key, input, auth_tag, out_b));
                require(a == b);
            }
            {
                std::array<std::byte, 16> iv {};
                std::array<std::byte, 17> input {};
                std::array<std::byte, 16> a, b;
                a.fill(std::byte {0x5a});
                b = a;
                require(
                    ctr_a->transform(iv, input, a) == Error::buffer_too_small);
                require(
                    ctr_b->transform(iv, input, b) == Error::buffer_too_small);
                require(a == b);
            }
            for (const std::size_t size :
                {0U, 1U, 15U, 16U, 17U, 1023U, 1024U, 1025U, 1316U, 4097U}) {
                std::vector<std::byte> plain(size, std::byte {0x29}), a(size),
                    b(size);
                std::array<std::byte, 16> iv;
                iv.fill(std::byte {0xff}); // Cross the full counter rollover.
                require(ctr_a->transform(iv, plain, a) == Error::none);
                require(
                    ctr_b->transform(iv, plain, b) == Error::none && a == b);
                require(
                    ctr_a->transform(iv, a, a) == Error::none && a == plain);
                for (std::size_t offset = 1; offset < 8; ++offset) {
                    std::vector<std::byte> unaligned(
                        size + offset + 1, std::byte {0x6d});
                    auto payload = std::span(unaligned).subspan(offset, size);
                    std::copy(plain.begin(), plain.end(), payload.begin());
                    require(
                        ctr_a->transform(iv, payload, payload) == Error::none);
                    require(
                        std::equal(payload.begin(), payload.end(), b.begin()));
                    require(unaligned.front() == std::byte {0x6d}
                        && unaligned.back() == std::byte {0x6d});
                }
                std::array<std::byte, 12> nonce {};
                std::array<std::byte, 16> tag_a {}, tag_b {};
                require(
                    gcm_a->seal(nonce, key, plain, a, tag_a) == Error::none);
                require(
                    gcm_b->seal(nonce, key, plain, b, tag_b) == Error::none);
                require(a == b && tag_a == tag_b);
                require(gcm_a->open(nonce, key, a, tag_a, a) == Error::none
                    && a == plain);
                a = plain;
                require(gcm_a->seal(nonce, key, a, a, tag_a) == Error::none
                    && a == b);
                tag_b[0] ^= std::byte {1};
                require(gcm_a->open(nonce, key, b, tag_b, a)
                    == Error::authentication_failure);
                require(std::all_of(a.begin(), a.end(), [](std::byte v) {
                    return v == std::byte {};
                }));
            }
        }
        std::cout << "BCrypt/OpenSSL comparisons passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
