// SPDX-License-Identifier: MIT
#pragma once
#include <mbedtls/version.h>

#if MBEDTLS_VERSION_NUMBER < 0x03060700 || MBEDTLS_VERSION_NUMBER >= 0x03070000
#error "Robotweax requires Mbed TLS 3.6.7 or later 3.6.x"
#endif
#if !defined(MBEDTLS_AES_C) || !defined(MBEDTLS_CIPHER_MODE_CTR)               \
    || !defined(MBEDTLS_GCM_C) || !defined(MBEDTLS_NIST_KW_C)                  \
    || !defined(MBEDTLS_CIPHER_C) || !defined(MBEDTLS_PKCS5_C)                 \
    || !defined(MBEDTLS_MD_C) || !defined(MBEDTLS_SHA1_C)                      \
    || !defined(MBEDTLS_SHA256_C) || !defined(MBEDTLS_ENTROPY_C)               \
    || !defined(MBEDTLS_CTR_DRBG_C)
#error "Mbed TLS is missing required Robotweax crypto modules"
#endif
#if defined(MBEDTLS_AES_ALT) || defined(MBEDTLS_GCM_ALT)                       \
    || defined(MBEDTLS_AES_SETKEY_ENC_ALT)                                     \
    || defined(MBEDTLS_AES_SETKEY_DEC_ALT) || defined(MBEDTLS_AES_ENCRYPT_ALT) \
    || defined(MBEDTLS_AES_DECRYPT_ALT) || defined(MBEDTLS_SHA1_ALT)           \
    || defined(MBEDTLS_SHA256_ALT) || defined(MBEDTLS_SHA512_ALT)              \
    || defined(MBEDTLS_PLATFORM_ZEROIZE_ALT) || defined(MBEDTLS_THREADING_ALT) \
    || defined(MBEDTLS_NIST_KW_ALT)                                            \
    || defined(MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH)                            \
    || defined(MBEDTLS_BLOCK_CIPHER_NO_DECRYPT)                                \
    || defined(MBEDTLS_USE_PSA_CRYPTO) || defined(MBEDTLS_TEST_NULL_ENTROPY)   \
    || defined(MBEDTLS_NO_PLATFORM_ENTROPY)                                    \
    || defined(MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES)                             \
    || defined(MBEDTLS_CTR_DRBG_USE_128_BIT_KEY)
#error                                                                         \
    "Robotweax qualifies built-in Mbed TLS AES-128/192/256 with legacy crypto APIs"
#endif
