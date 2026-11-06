// SPDX-License-Identifier: MIT
// Fixed public inputs exercise the legacy encoder, never peer-authentication records.
// Compare stdout across installed builds; no random values or normalization.
#include "robotweax/srt/handshake_datagram.hpp"
#include <array>
#include <cstdio>
using namespace robotweax::srt;
int main()
{
    for (const bool session : {false, true})
        for (const bool gcm : {false, true})
            for (const unsigned length : {16U, 24U, 32U})
                for (unsigned phase = 0; phase < 3; ++phase) {
                    HandshakeAction action;
                    action.kind = HandshakeActionKind::send;
                    action.packet.version = 5;
                    action.packet.socket_id = phase == 1 ? 456 : 123;
                    action.packet.initial_sequence = SequenceNumber {1000};
                    action.packet.syn_cookie = 0x12345678;
                    action.packet.encryption_field =
                        encryption_field_for_key_length(length);
                    action.packet.request = phase == 2
                        ? HandshakeRequest::agreement
                        : HandshakeRequest::conclusion;
                    action.packet.extension_field = static_cast<std::uint16_t>(
                        (phase != 2 ? 3 : 0) | (session || phase == 0 ? 4 : 0));
                    action.has_handshake_extension = phase != 2;
                    action.extension_type = phase == 1
                        ? HandshakeExtensionType::handshake_response
                        : HandshakeExtensionType::handshake_request;
                    if (phase == 0) {
                        action.has_stream_id_extension = true;
                        if (!action.stream_id.assign("legacy-wire-fixture"))
                            return 1;
                    }
                    if (phase != 2) {
                        std::array<std::byte, 16> salt {};
                        std::array<std::byte, 40> wrapped {};
                        for (unsigned i = 0; i < salt.size(); ++i)
                            salt[i] = std::byte(i);
                        for (unsigned i = 0; i < wrapped.size(); ++i)
                            wrapped[i] = std::byte(i + 32);
                        const auto material = encode_key_material(
                            {.keys = EncryptionKey::even,
                                .cipher = gcm ? KeyMaterialCipher::aes_gcm
                                              : KeyMaterialCipher::aes_ctr,
                                .authentication = gcm
                                    ? KeyMaterialAuthentication::aes_gcm
                                    : KeyMaterialAuthentication::none,
                                .salt = salt,
                                .key_length = length,
                                .wrapped_keys =
                                    std::span {wrapped}.first(length + 8)},
                            action.key_material.bytes);
                        if (!material)
                            return 2;
                        action.key_material.size = material.bytes_written;
                        action.has_key_material_extension = true;
                        action.key_material_extension_type = phase == 1
                            ? HandshakeExtensionType::key_material_response
                            : HandshakeExtensionType::key_material_request;
                    }
                    action.has_session_authentication = session;
                    for (unsigned i = 0; i < 32; ++i) {
                        action.session_authentication.caller_nonce[i] =
                            std::byte(i + 1);
                        if (phase != 0) {
                            action.session_authentication.listener_nonce[i] =
                                std::byte(i + 33);
                            action.session_authentication.proof[i] =
                                std::byte(i + 65);
                        }
                    }
                    std::array<std::byte, 1500> bytes {};
                    const auto encoded = encode_handshake_datagram(action,
                        PacketTimestamp {77}, phase == 1 ? 123 : 456, bytes);
                    if (!encoded
                        || !decode_handshake_datagram(
                            std::span {bytes}.first(encoded.bytes_written)))
                        return 3;
                    std::printf("%d %d %u %u ", session, gcm, length, phase);
                    for (const auto byte :
                        std::span {bytes}.first(encoded.bytes_written))
                        std::printf("%02x", std::to_integer<unsigned>(byte));
                    std::puts("");
                }
}
