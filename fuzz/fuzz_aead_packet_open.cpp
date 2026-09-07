#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/crypto.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

using namespace robotweax::srt;

namespace {

constexpr std::size_t maximum_fuzz_plaintext_size = 256U;
constexpr std::size_t fuzz_maximum_segment_size = 1'500U;
constexpr std::size_t fuzz_transport_overhead_size = 44U;

[[noreturn]] void fail_invariant() noexcept
{
    std::abort();
}

void require_invariant(bool condition) noexcept
{
    if (!condition) {
        fail_invariant();
    }
}

[[nodiscard]] bool all_bytes_equal(
    std::span<const std::byte> bytes, std::byte expected) noexcept
{
    return std::all_of(bytes.begin(), bytes.end(), [expected](std::byte value) {
        return value == expected;
    });
}

struct AeadFixture {
    CryptoConfiguration configuration {
        .passphrase = "authenticated ingress fuzz fixture",
        .mode = CryptoMode::aes_gcm,
        .enable_aes_gcm = true,
        .key_length = 16U,
    };
    CryptoSession sender {configuration};
    CryptoSession receiver {configuration};

    AeadFixture() noexcept
    {
        require_invariant(sender.start_initiator() == Error::none);
        require_invariant(
            receiver.accept_key_material(sender.pending_key_material(), true)
            == Error::none);
        require_invariant(sender.acknowledge_key_material(
                              receiver.key_material_response(), true)
            == Error::none);
        require_invariant(!sender.ready_to_send_data());
        require_invariant(!receiver.ready_to_send_data());

        // Direct-session fixtures emulate the runtime confirmation of each
        // independent DATA direction before exercising authenticated packets.
        require_invariant(
            receiver.accept_key_material(sender.pending_key_material(), false)
            == Error::none);
        require_invariant(sender.acknowledge_key_material(
                              receiver.key_material_response(), false)
            == Error::none);
        require_invariant(
            sender.accept_key_material(receiver.pending_key_material(), false)
            == Error::none);
        require_invariant(receiver.acknowledge_key_material(
                              sender.key_material_response(), false)
            == Error::none);
        require_invariant(sender.ready_to_send_data());
        require_invariant(receiver.ready_to_send_data());
        require_invariant(sender.authenticated_data_enabled());
        require_invariant(receiver.authenticated_data_enabled());
    }
};

[[nodiscard]] AeadFixture& fixture() noexcept
{
    static AeadFixture value;
    return value;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span {data, size});
    (void)decode_packet(input);

    AeadFixture& aead = fixture();
    const std::uint8_t control = size == 0U ? 0U : data[0];
    const std::size_t plaintext_size =
        std::min(size > 1U ? size - 1U : 0U, maximum_fuzz_plaintext_size);
    std::array<std::byte, maximum_fuzz_plaintext_size> plaintext {};
    if (plaintext_size != 0U) {
        std::copy_n(input.begin() + 1, plaintext_size, plaintext.begin());
    }

    const CryptoPayloadBudget budget =
        make_crypto_payload_budget(CryptoMode::aes_gcm,
            fuzz_maximum_segment_size, fuzz_transport_overhead_size);
    require_invariant(static_cast<bool>(budget));

    DataHeader header {
        .sequence = SequenceNumber {7U},
        .message_number = 19U,
        .boundary = MessageBoundary::solo,
        .in_order = true,
        .encryption_key = aead.sender.active_sender_key(),
        .timestamp = PacketTimestamp {0x1122'3344U},
        .destination_socket_id = 0x5566'7788U,
    };
    std::array<std::byte, maximum_data_payload_size> protected_storage {};
    const auto prepared =
        prepare_protected_payload(protected_storage, plaintext_size, budget);
    require_invariant(static_cast<bool>(prepared));

    EncryptionKey key = EncryptionKey::none;
    require_invariant(
        aead.sender.seal(header, std::span {plaintext}.first(plaintext_size),
            prepared.payload.ciphertext, prepared.payload.authentication_tag,
            key)
        == Error::none);
    require_invariant(key == header.encryption_key);

    std::array<std::byte, packet_header_size + maximum_data_payload_size>
        datagram {};
    const MutablePacketView packet {
        .kind = PacketKind::data,
        .data = header,
        .payload = std::span {protected_storage}.first(prepared.bytes_written),
    };
    const auto encoded = encode_packet(packet, datagram);
    require_invariant(static_cast<bool>(encoded));
    std::size_t datagram_size = encoded.bytes_written;

    const std::uint8_t mutation = control % 12U;
    bool expect_success = mutation <= 1U;
    switch (mutation) {
    case 0U:
        break;
    case 1U:
        // RETRANSMITTED is transport metadata, not authenticated DATA identity.
        datagram[4] ^= std::byte {0x04};
        break;
    case 2U:
        datagram[3] ^= std::byte {0x01};
        break;
    case 3U:
        datagram[7] ^= std::byte {0x01};
        break;
    case 4U:
        datagram[11] ^= std::byte {0x01};
        break;
    case 5U:
        datagram[15] ^= std::byte {0x01};
        break;
    case 6U:
        // Change EVEN to ODD without changing the encrypted bytes.
        datagram[4] ^= std::byte {0x18};
        break;
    case 7U:
        datagram[packet_header_size] ^= std::byte {0x01};
        break;
    case 8U:
        datagram[datagram_size - 1U] ^= std::byte {0x01};
        break;
    case 9U:
        --datagram_size;
        break;
    case 10U:
        if (plaintext_size == 0U) {
            datagram[datagram_size - 1U] ^= std::byte {0x01};
        }
        break;
    case 11U:
        // Retain valid wire bytes and exercise a larger caller buffer below.
        break;
    default:
        fail_invariant();
    }

    const auto decoded =
        decode_packet(std::span {datagram}.first(datagram_size));
    require_invariant(static_cast<bool>(decoded));
    require_invariant(decoded.packet.kind == PacketKind::data);
    const auto protected_payload =
        decode_protected_payload(decoded.packet.payload, budget);
    if (!protected_payload) {
        require_invariant(mutation == 9U);
        return 0;
    }

    std::array<std::byte, maximum_fuzz_plaintext_size + 1U> opened {};
    opened.fill(std::byte {0xa5});
    std::size_t output_size = protected_payload.payload.ciphertext.size();
    if (mutation == 10U && output_size != 0U) {
        --output_size;
        expect_success = false;
    } else if (mutation == 11U) {
        output_size = std::min(output_size + 1U, opened.size());
        expect_success = true;
    }
    const auto output = std::span {opened}.first(output_size);
    const Error result = aead.receiver.open(decoded.packet.data,
        protected_payload.payload.ciphertext,
        protected_payload.payload.authentication_tag, output);

    if (expect_success) {
        require_invariant(result == Error::none);
        require_invariant(
            protected_payload.payload.ciphertext.size() == plaintext_size);
        require_invariant(
            std::equal(opened.begin(), opened.begin() + plaintext_size,
                plaintext.begin(), plaintext.begin() + plaintext_size));
        require_invariant(all_bytes_equal(
            std::span {opened}.subspan(plaintext_size), std::byte {0xa5}));
    } else {
        require_invariant(result != Error::none);
        require_invariant(all_bytes_equal(output, std::byte {0}));
        require_invariant(all_bytes_equal(
            std::span {opened}.subspan(output_size), std::byte {0xa5}));
    }
    return 0;
}
