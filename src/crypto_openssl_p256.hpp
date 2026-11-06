// SPDX-License-Identifier: MIT
#pragma once

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_PEERAUTH
// Private OpenSSL provider implementation, included only with PEERAUTH enabled.
#include "robotweax/srt/crypto_provider.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/param_build.h>

#include <algorithm>
#include <array>
#include <new>
#include <utility>

namespace robotweax::srt::openssl_peer_auth {
template <class T, auto Free> struct Deleter {
    void operator()(T* value) const noexcept
    {
        Free(value);
    }
};
template <class T, auto Free>
using Owned = std::unique_ptr<T, Deleter<T, Free>>;
using Key = Owned<EVP_PKEY, EVP_PKEY_free>;
using Context = Owned<EVP_PKEY_CTX, EVP_PKEY_CTX_free>;
using Big = Owned<BIGNUM, BN_clear_free>;
using Signature = Owned<ECDSA_SIG, ECDSA_SIG_free>;
struct ParametersDeleter {
    void operator()(OSSL_PARAM* params) const noexcept
    {
        if (params) {
            auto* scalar = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_PRIV_KEY);
            if (scalar)
                OPENSSL_cleanse(scalar->data, scalar->data_size);
            OSSL_PARAM_free(params);
        }
    }
};
using Parameters = std::unique_ptr<OSSL_PARAM, ParametersDeleter>;

inline void erase(std::span<std::byte> output) noexcept
{
    OPENSSL_cleanse(output.data(), output.size());
}
template <std::size_t N>
inline const unsigned char* bytes_ptr(
    std::span<const std::byte, N> input) noexcept
{
    return reinterpret_cast<const unsigned char*>(input.data());
}
template <std::size_t N>
inline unsigned char* bytes_ptr(std::span<std::byte, N> output) noexcept
{
    return reinterpret_cast<unsigned char*>(output.data());
}
inline Error import_public(
    std::span<const std::byte, 64> point, Key& result) noexcept
{
    result.reset();
    std::array<unsigned char, 65> encoded {};
    encoded[0] = POINT_CONVERSION_UNCOMPRESSED;
    std::copy_n(bytes_ptr(point), 64, encoded.begin() + 1);
    char group[] = "prime256v1";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
        OSSL_PARAM_construct_octet_string(
            OSSL_PKEY_PARAM_PUB_KEY, encoded.data(), encoded.size()),
        OSSL_PARAM_construct_end()};
    Context context(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
    if (!context || EVP_PKEY_fromdata_init(context.get()) != 1)
        return Error::cryptographic_failure;
    EVP_PKEY* raw = nullptr;
    const int imported =
        EVP_PKEY_fromdata(context.get(), &raw, EVP_PKEY_PUBLIC_KEY, params);
    Key key(raw);
    if (imported != 1 || !key)
        return Error::invalid_key_material;
    Context check(EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr));
    if (!check)
        return Error::cryptographic_failure;
    // Full public check rejects infinity, non-curve and out-of-range coordinates.
    if (EVP_PKEY_public_check(check.get()) != 1)
        return Error::invalid_key_material;
    result = std::move(key);
    return Error::none;
}
inline Error import_private(
    std::span<const std::byte, 32> scalar, Key& result) noexcept
{
    result.reset();
    Owned<EC_GROUP, EC_GROUP_free> group(
        EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
    Big secret(BN_secure_new());
    Owned<BN_CTX, BN_CTX_free> bn_context(BN_CTX_secure_new());
    if (!group || !secret || !bn_context)
        return Error::cryptographic_failure;
    if (!BN_bin2bn(bytes_ptr(scalar), 32, secret.get()))
        return Error::cryptographic_failure;
    if (BN_is_zero(secret.get())
        || BN_cmp(secret.get(), EC_GROUP_get0_order(group.get())) >= 0)
        return Error::invalid_key_material;
    BN_set_flags(secret.get(), BN_FLG_CONSTTIME);
    Owned<EC_POINT, EC_POINT_clear_free> point(EC_POINT_new(group.get()));
    std::array<unsigned char, 65> encoded {};
    if (!point
        || EC_POINT_mul(group.get(), point.get(), secret.get(), nullptr,
               nullptr, bn_context.get())
            != 1
        || EC_POINT_point2oct(group.get(), point.get(),
               POINT_CONVERSION_UNCOMPRESSED, encoded.data(), encoded.size(),
               bn_context.get())
            != encoded.size())
        return Error::cryptographic_failure;
    Owned<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free> builder(OSSL_PARAM_BLD_new());
    if (!builder
        || OSSL_PARAM_BLD_push_utf8_string(
               builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0)
            != 1
        || OSSL_PARAM_BLD_push_BN(
               builder.get(), OSSL_PKEY_PARAM_PRIV_KEY, secret.get())
            != 1
        || OSSL_PARAM_BLD_push_octet_string(builder.get(),
               OSSL_PKEY_PARAM_PUB_KEY, encoded.data(), encoded.size())
            != 1)
        return Error::cryptographic_failure;
    Parameters params(OSSL_PARAM_BLD_to_param(builder.get()));
    Context context(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
    if (!params || !context || EVP_PKEY_fromdata_init(context.get()) != 1)
        return Error::cryptographic_failure;
    EVP_PKEY* raw = nullptr;
    const int imported =
        EVP_PKEY_fromdata(context.get(), &raw, EVP_PKEY_KEYPAIR, params.get());
    Key key(raw);
    if (imported != 1 || !key)
        return Error::cryptographic_failure;
    Context check(EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr));
    if (!check || EVP_PKEY_private_check(check.get()) != 1
        || EVP_PKEY_pairwise_check(check.get()) != 1)
        return Error::cryptographic_failure;
    result = std::move(key);
    return Error::none;
}
class PrivateKey final : public P256Key {
public:
    explicit PrivateKey(Key key) noexcept
        : key_(std::move(key))
    {
    }
    Error public_key(std::span<std::byte, 64> output) noexcept override
    {
        std::array<unsigned char, 65> encoded {};
        std::size_t size = 0;
        if (EVP_PKEY_get_octet_string_param(key_.get(), OSSL_PKEY_PARAM_PUB_KEY,
                encoded.data(), encoded.size(), &size)
                != 1
            || size != encoded.size()
            || encoded[0] != POINT_CONVERSION_UNCOMPRESSED) {
            erase(output);
            return Error::cryptographic_failure;
        }
        std::copy_n(reinterpret_cast<const std::byte*>(encoded.data() + 1), 64,
            output.begin());
        return Error::none;
    }
    Error agree(std::span<const std::byte, 64> peer,
        std::span<std::byte, 32> output) noexcept override
    {
        Key peer_key;
        const auto error = import_public(peer, peer_key);
        if (error != Error::none) {
            erase(output);
            return error;
        }
        Context context(
            EVP_PKEY_CTX_new_from_pkey(nullptr, key_.get(), nullptr));
        std::size_t size = output.size();
        if (!context || EVP_PKEY_derive_init(context.get()) != 1
            || EVP_PKEY_derive_set_peer(context.get(), peer_key.get()) != 1
            || EVP_PKEY_derive(context.get(), bytes_ptr(output), &size) != 1
            || size != output.size()) {
            erase(output);
            return Error::cryptographic_failure;
        }
        return Error::none;
    }
    Error sign_sha256(std::span<const std::byte> message,
        std::span<std::byte, 64> output) noexcept override
    {
        Owned<EVP_MD_CTX, EVP_MD_CTX_free> context(EVP_MD_CTX_new());
        std::array<unsigned char, 72> encoded {};
        std::size_t size = encoded.size();
        EVP_PKEY_CTX* signing = nullptr;
        if (!context
            || EVP_DigestSignInit(
                   context.get(), &signing, EVP_sha256(), nullptr, key_.get())
                != 1) {
            erase(output);
            return Error::cryptographic_failure;
        }
#ifdef OSSL_SIGNATURE_PARAM_NONCE_TYPE
        unsigned int nonce_type = 1; // RFC 6979, available since OpenSSL 3.2.
        OSSL_PARAM parameters[] = {
            OSSL_PARAM_construct_uint(
                OSSL_SIGNATURE_PARAM_NONCE_TYPE, &nonce_type),
            OSSL_PARAM_construct_end()};
        const auto* supported = EVP_PKEY_CTX_settable_params(signing);
        if (supported
            && OSSL_PARAM_locate_const(
                   supported, OSSL_SIGNATURE_PARAM_NONCE_TYPE)
                != nullptr
            && EVP_PKEY_CTX_set_params(signing, parameters) != 1) {
            erase(output);
            return Error::cryptographic_failure;
        }
#endif
        if (EVP_DigestSign(context.get(), encoded.data(), &size,
                bytes_ptr(message), message.size())
            != 1) {
            erase(output);
            return Error::cryptographic_failure;
        }
        const auto* cursor = encoded.data();
        Signature signature(
            d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(size)));
        if (!signature || cursor != encoded.data() + size
            || BN_bn2binpad(ECDSA_SIG_get0_r(signature.get()),
                   bytes_ptr(output.first<32>()), 32)
                != 32
            || BN_bn2binpad(ECDSA_SIG_get0_s(signature.get()),
                   bytes_ptr(output.last<32>()), 32)
                != 32) {
            erase(output);
            return Error::cryptographic_failure;
        }
        return Error::none;
    }

private:
    Key key_;
};
inline Error own(Key key, std::unique_ptr<P256Key>& output) noexcept
{
    output.reset(new (std::nothrow) PrivateKey(std::move(key)));
    return output ? Error::none : Error::cryptographic_failure;
}
inline Error generate(std::unique_ptr<P256Key>& output) noexcept
{
    output.reset();
    Context context(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1
        || EVP_PKEY_CTX_set_group_name(context.get(), "prime256v1") != 1)
        return Error::cryptographic_failure;
    EVP_PKEY* raw = nullptr;
    const int generated = EVP_PKEY_generate(context.get(), &raw);
    Key key(raw);
    if (generated != 1 || !key)
        return Error::cryptographic_failure;
    return own(std::move(key), output);
}
inline Error verify(std::span<const std::byte, 64> public_key,
    std::span<const std::byte> message,
    std::span<const std::byte, 64> raw) noexcept
{
    Key key;
    const auto error = import_public(public_key, key);
    if (error != Error::none)
        return error;
    Signature signature(ECDSA_SIG_new());
    Big r(BN_bin2bn(bytes_ptr(raw.first<32>()), 32, nullptr));
    Big s(BN_bin2bn(bytes_ptr(raw.last<32>()), 32, nullptr));
    if (!signature || !r || !s)
        return Error::cryptographic_failure;
    if (ECDSA_SIG_set0(signature.get(), r.get(), s.get()) != 1)
        return Error::cryptographic_failure;
    (void)r.release();
    (void)s.release();
    std::array<unsigned char, 72> encoded {};
    auto* cursor = encoded.data();
    const int size = i2d_ECDSA_SIG(signature.get(), &cursor);
    if (size <= 0 || static_cast<std::size_t>(size) > encoded.size())
        return Error::cryptographic_failure;
    Owned<EVP_MD_CTX, EVP_MD_CTX_free> context(EVP_MD_CTX_new());
    if (!context
        || EVP_DigestVerifyInit(
               context.get(), nullptr, EVP_sha256(), nullptr, key.get())
            != 1)
        return Error::cryptographic_failure;
    const auto result = EVP_DigestVerify(context.get(), encoded.data(),
        static_cast<std::size_t>(size), bytes_ptr(message), message.size());
    return result == 1 ? Error::none
        : result == 0  ? Error::authentication_failure
                       : Error::cryptographic_failure;
}
} // namespace robotweax::srt::openssl_peer_auth

#endif
