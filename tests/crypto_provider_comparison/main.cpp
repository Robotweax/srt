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
