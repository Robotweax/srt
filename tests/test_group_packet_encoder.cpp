#include "test.hpp"

#include "compat/group_multicast_emitter.hpp"
#include "compat/group_packet_encoder.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "robotweax/srt/connection_group.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {
using Encoder = GroupPacketEncoder;
using Status = Encoder::Status;
constexpr std::uint32_t group_id = group_handle_mask | 53U;
constexpr std::uint64_t stream_generation = 7;
constexpr SequenceNumber initial {SequenceNumber::mask - 2U};
constexpr std::array<std::byte, 5> message {std::byte {'h'}, std::byte {'e'},
    std::byte {'l'}, std::byte {'l'}, std::byte {'o'}};

DataHeader header(SequenceNumber sequence = initial)
{
    return {.sequence = sequence,
        .message_number = 0x03ff'ffffU,
        .boundary = MessageBoundary::solo,
        .in_order = true,
        .timestamp = PacketTimestamp {0xffff'fff0U},
        .destination_socket_id = group_id};
}

Encoder::Configuration configuration(
    std::shared_ptr<SharedTransmitKeySchedule> schedule = {})
{
    return {.group_id = group_id,
        .stream_generation = stream_generation,
        .initial_sequence = initial,
        .key_schedule = std::move(schedule)};
}

std::shared_ptr<SharedTransmitKeySchedule> schedule(
    CryptoMode mode, std::size_t key_length = 16, std::uint64_t refresh = 4)
{
    auto result = std::make_shared<SharedTransmitKeySchedule>(
        SharedTransmitKeySchedule::Configuration {.key_length = key_length,
            .mode = mode,
            .refresh_rate_packets = refresh,
            .preannouncement_packets = 1,
            .origin = initial});
    REQUIRE_EQ(result->initialize(), Error::none);
    return result;
}

// An ordinary member's post-handshake directional KM exchange. No provider
// internals or independently selected keys are used as the comparison oracle.
struct Member {
    CryptoConfiguration config;
    CryptoSession initiator;
    CryptoSession sender;
    CryptoSession receiver;
    Member(std::shared_ptr<SharedTransmitKeySchedule> keys)
        : config {.passphrase = "canonical multicast fixture",
              .mode = keys->mode(),
              .enable_aes_gcm = true,
              .key_length = keys->key_length(),
              .refresh_rate_packets = 4,
              .preannouncement_packets = 1}
        , initiator(config)
        , sender(config)
        , receiver(config)
    {
        REQUIRE_EQ(initiator.start_initiator(), Error::none);
        REQUIRE_EQ(
            sender.accept_key_material(initiator.pending_key_material(), true),
            Error::none);
        REQUIRE_EQ(sender.attach_shared_transmit_schedule(std::move(keys)),
            Error::none);
        exchange();
    }
    void exchange()
    {
        REQUIRE(!sender.pending_key_material().empty());
        REQUIRE_EQ(
            receiver.accept_key_material(sender.pending_key_material(), false),
            Error::none);
        REQUIRE_EQ(sender.acknowledge_key_material(
                       receiver.key_material_response(), false),
            Error::none);
        REQUIRE(sender.ready_to_send_data());
    }
    std::vector<std::byte> encode(
        DataHeader identity, std::span<const std::byte> plaintext)
    {
        const auto revision = sender.key_material_revision();
        REQUIRE_EQ(
            sender.prepare_data_sequence(identity.sequence), Error::none);
        if (sender.key_material_revision() != revision)
            exchange();
        identity.encryption_key = sender.sender_key_for(identity.sequence);
        std::vector<std::byte> bytes(packet_header_size + plaintext.size()
            + (config.mode == CryptoMode::aes_gcm
                    ? srt_gcm_authentication_tag_size
                    : 0U));
        REQUIRE(
            encode_packet({.kind = PacketKind::data, .data = identity}, bytes));
        auto ciphertext =
            std::span {bytes}.subspan(packet_header_size, plaintext.size());
        EncryptionKey selected = EncryptionKey::none;
        if (config.mode == CryptoMode::aes_gcm) {
            REQUIRE_EQ(
                sender.seal(identity, plaintext, ciphertext,
                    std::span {bytes}.last(srt_gcm_authentication_tag_size),
                    selected),
                Error::none);
        } else {
            REQUIRE_EQ(sender.encrypt(
                           identity.sequence, plaintext, ciphertext, selected),
                Error::none);
        }
        REQUIRE_EQ(selected, identity.encryption_key);
        return bytes;
    }
    void decrypt(
        const Encoder::Packet& packet, std::span<const std::byte> expected)
    {
        const auto decoded = decode_packet(packet.bytes());
        REQUIRE(decoded);
        std::vector<std::byte> plaintext(expected.size());
        if (config.mode == CryptoMode::aes_gcm) {
            REQUIRE_EQ(receiver.open(decoded.packet.data,
                           decoded.packet.payload.first(expected.size()),
                           decoded.packet.payload.last(
                               srt_gcm_authentication_tag_size),
                           plaintext),
                Error::none);
        } else {
            REQUIRE_EQ(receiver.decrypt(decoded.packet.data.encryption_key,
                           decoded.packet.data.sequence, decoded.packet.payload,
                           plaintext),
                Error::none);
            receiver.note_accepted_receive_sequence(
                decoded.packet.data.sequence);
        }
        REQUIRE(std::equal(plaintext.begin(), plaintext.end(), expected.begin(),
            expected.end()));
    }
};
} // namespace

TEST(compat_group_encoder_rejects_invalid_configuration)
{
    for (unsigned change = 0; change < 9; ++change) {
        auto c = configuration();
        switch (change) {
        case 0:
            c.group_id = 53;
            break;
        case 1:
            c.stream_generation = 0;
            break;
        case 2:
            c.packet_capacity = 0;
            break;
        case 3:
            c.packet_capacity = Encoder::maximum_packets + 1;
            break;
        case 4:
            c.generation_capacity = 0;
            break;
        case 5:
            c.generation_capacity = Encoder::maximum_generations + 1;
            break;
        case 6:
            c.family = static_cast<IpAddressFamily>(99);
            break;
        case 7:
            c.maximum_transmission_unit = 44;
            break;
        case 8:
            c.key_schedule = std::make_shared<SharedTransmitKeySchedule>(
                SharedTransmitKeySchedule::Configuration {});
            break;
        }
        REQUIRE(!Encoder::create(c));
    }
}

TEST(compat_group_encoder_plaintext_preserves_fragment_identity_and_wraps)
{
    auto encoder = Encoder::create(configuration());
    REQUIRE(encoder);
    for (unsigned index = 0; index < 5; ++index) {
        auto identity = header(initial.advanced(index));
        identity.boundary = static_cast<MessageBoundary>(index % 4U);
        identity.timestamp = PacketTimestamp {0xffff'fff0U + index * 16U};
        identity.in_order = index % 2U == 0U;
        auto prepared = encoder->prepare(identity, message);
        REQUIRE(prepared);
        std::array<std::byte, 64> expected {};
        const auto encoded = encode_packet(
            {.kind = PacketKind::data, .data = identity, .payload = message},
            expected);
        REQUIRE(encoded);
        REQUIRE(std::equal(prepared.packet->bytes().begin(),
            prepared.packet->bytes().end(), expected.begin(),
            expected.begin() + encoded.bytes_written));
        REQUIRE_EQ(prepared.packet->stream_generation(), stream_generation);
        REQUIRE(!prepared.packet->crypto_generation().has_value());
        REQUIRE(!prepared.packet->generation_owner());
        REQUIRE(encoder->commit(prepared.packet));
        REQUIRE(!encoder->commit(prepared.packet));
    }
    REQUIRE_EQ(encoder->snapshot().next_sequence, initial.advanced(5));
    REQUIRE_EQ(encoder->snapshot().committed_packets, 5U);
}

TEST(compat_group_encoder_retries_are_one_immutable_preparation_until_commit)
{
    auto encoder =
        Encoder::create(configuration(schedule(CryptoMode::aes_gcm)));
    REQUIRE(encoder);
    auto identity = header();
    auto payload = message;
    auto first = encoder->prepare(identity, payload);
    REQUIRE(first);
    REQUIRE_EQ(encoder->snapshot().next_sequence, initial);
    REQUIRE_EQ(encoder->snapshot().committed_packets, 0U);
    REQUIRE(encoder->snapshot().prepared);
    for (unsigned change = 0; change < 6; ++change) {
        auto changed = identity;
        auto bytes = payload;
        switch (change) {
        case 0:
            changed.message_number = 1;
            break;
        case 1:
            changed.boundary = MessageBoundary::first;
            break;
        case 2:
            changed.in_order = false;
            break;
        case 3:
            changed.timestamp = PacketTimestamp {1};
            break;
        case 4:
            bytes[0] ^= std::byte {1};
            break;
        case 5:
            bytes[4] ^= std::byte {1};
            break;
        }
        REQUIRE_EQ(encoder->prepare(changed, bytes).status,
            Status::conflicting_packet);
    }
    auto again = encoder->prepare(identity, payload);
    REQUIRE(again);
    REQUIRE_EQ(again.packet, first.packet);
    auto other = Encoder::create(configuration());
    REQUIRE(other);
    auto foreign = other->prepare(identity, payload);
    REQUIRE(foreign);
    REQUIRE(!encoder->commit(foreign.packet));
    REQUIRE(!encoder->commit({}));
    REQUIRE(encoder->commit(first.packet));
    REQUIRE_EQ(
        encoder->prepare(identity, payload).status, Status::wrong_sequence);
    std::fill(payload.begin(), payload.end(), std::byte {});
    REQUIRE_EQ(first.packet->header().timestamp, identity.timestamp);
}

TEST(compat_group_encoder_rejects_invalid_identity_before_key_frontier_mutation)
{
    auto keys = schedule(CryptoMode::aes_gcm);
    auto encoder = Encoder::create(configuration(keys));
    REQUIRE(encoder);
    for (unsigned change = 0; change < 7; ++change) {
        auto identity = header();
        switch (change) {
        case 0:
            identity.retransmitted = true;
            break;
        case 1:
            identity.encryption_key = EncryptionKey::even;
            break;
        case 2:
            identity.destination_socket_id ^= 1U;
            break;
        case 3:
            identity.message_number = 0;
            break;
        case 4:
            identity.message_number = 0x0400'0000U;
            break;
        case 5:
            identity.boundary = static_cast<MessageBoundary>(4);
            break;
        case 6:
            identity.sequence = initial.advanced(100);
            break;
        }
        REQUIRE_EQ(encoder->prepare(identity, message).status,
            change == 6 ? Status::wrong_sequence : Status::invalid_packet);
    }
    REQUIRE_EQ(encoder->prepare(header(), {}).status, Status::invalid_packet);
    REQUIRE_EQ(keys->frontier_position(), 0U);
    REQUIRE_EQ(encoder->snapshot().live_packets, 0U);
}

TEST(compat_group_encoder_mtu_budget_reserves_complete_gcm_tag_in_both_families)
{
    for (const auto family : {IpAddressFamily::ipv4, IpAddressFamily::ipv6}) {
        for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
            auto c = configuration(schedule(mode));
            c.family = family;
            c.maximum_transmission_unit = 200;
            auto encoder = Encoder::create(c);
            REQUIRE(encoder);
            const auto overhead = packet_header_size
                + (family == IpAddressFamily::ipv4 ? 28U : 48U);
            const auto tag = mode == CryptoMode::aes_gcm
                ? srt_gcm_authentication_tag_size
                : 0U;
            std::vector<std::byte> bytes(
                200U - overhead - tag, std::byte {0x3a});
            auto oversize = bytes;
            oversize.push_back(std::byte {});
            REQUIRE_EQ(encoder->prepare(header(), oversize).status,
                Status::invalid_packet);
            auto prepared = encoder->prepare(header(), bytes);
            REQUIRE(prepared);
            REQUIRE_EQ(prepared.packet->bytes().size(),
                200U - overhead + packet_header_size);
        }
    }
}

TEST(
    compat_group_encoder_live_packet_capacity_applies_to_external_history_owners)
{
    auto c = configuration();
    c.packet_capacity = 1;
    auto encoder = Encoder::create(c);
    REQUIRE(encoder);
    auto first = encoder->prepare(header(), message);
    REQUIRE(first);
    REQUIRE(encoder->commit(first.packet));
    REQUIRE_EQ(
        encoder->prepare(header(initial.next()), message).status, Status::full);
    REQUIRE_EQ(encoder->snapshot().next_sequence, initial.next());
    first.packet.reset();
    REQUIRE_EQ(encoder->snapshot().live_packets, 0U);
    REQUIRE(encoder->prepare(header(initial.next()), message));
}

TEST(
    compat_group_encoder_generation_capacity_blocks_rotation_until_emitter_lease_retires)
{
    auto keys = schedule(CryptoMode::aes_gcm, 16, 2);
    auto c = configuration(keys);
    c.generation_capacity = 1;
    auto encoder = Encoder::create(c);
    REQUIRE(encoder);
    auto first = encoder->prepare(header(), message);
    REQUIRE(first);
    auto retained = first.packet->generation_owner();
    REQUIRE(encoder->commit(first.packet));
    first.packet.reset();
    auto second = encoder->prepare(header(initial.next()), message);
    REQUIRE(second);
    REQUIRE(encoder->commit(second.packet));
    second.packet.reset();
    REQUIRE_EQ(encoder->prepare(header(initial.advanced(2)), message).status,
        Status::full);
    REQUIRE_EQ(keys->frontier_position(), 1U);
    REQUIRE_EQ(encoder->snapshot().committed_packets, 2U);
    retained.reset();
    auto next = encoder->prepare(header(initial.advanced(2)), message);
    REQUIRE(next);
    REQUIRE_EQ(
        next.packet->crypto_generation(), std::optional<std::uint64_t> {1});
}

TEST(
    compat_group_encoder_ctr_and_gcm_match_member_packets_through_rotations_and_wrap)
{
    for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        for (const std::size_t length : {16U, 24U, 32U}) {
            auto keys = schedule(mode, length);
            auto encoder = Encoder::create(configuration(keys));
            REQUIRE(encoder);
            Member member(keys);
            std::shared_ptr<const Encoder::Packet> oldest;
            std::vector<std::byte> oldest_bytes;
            for (unsigned i = 0; i < 24; ++i) {
                auto identity = header(initial.advanced(i));
                identity.timestamp = PacketTimestamp {0xffff'fff0U + i * 16U};
                identity.message_number = 1U + i;
                identity.boundary = static_cast<MessageBoundary>(i % 4U);
                const auto expected = member.encode(identity, message);
                auto prepared = encoder->prepare(identity, message);
                REQUIRE(prepared);
                REQUIRE_EQ(prepared.packet->crypto_generation(),
                    std::optional<std::uint64_t> {i / 4U});
                REQUIRE(std::equal(prepared.packet->bytes().begin(),
                    prepared.packet->bytes().end(), expected.begin(),
                    expected.end()));
                member.decrypt(*prepared.packet, message);
                REQUIRE(encoder->commit(prepared.packet));
                if (i == 0) {
                    oldest = prepared.packet;
                    oldest_bytes = expected;
                }
            }
            // The schedule's ring no longer owns generation zero. The
            // canonical packet still owns that immutable generation and bytes.
            std::array<std::byte, maximum_aes_key_size> old_key {};
            REQUIRE_EQ(keys->key_for(0, old_key), Error::invalid_state);
            REQUIRE(oldest->generation_owner());
            REQUIRE(std::equal(oldest->bytes().begin(), oldest->bytes().end(),
                oldest_bytes.begin(), oldest_bytes.end()));
            REQUIRE(encoder->snapshot().cached_generations
                <= Encoder::maximum_generations);
        }
    }
}

TEST(compat_group_encoder_gcm_keeps_dispatch_identity_on_unicast_repair)
{
    auto keys = schedule(CryptoMode::aes_gcm);
    Member member(keys);
    auto encoder = Encoder::create(configuration(keys));
    REQUIRE(encoder);
    auto prepared = encoder->prepare(header(), message);
    REQUIRE(prepared);
    const auto decoded = decode_packet(prepared.packet->bytes());
    REQUIRE(decoded);
    const auto ciphertext = decoded.packet.payload.first(message.size());
    const auto tag =
        decoded.packet.payload.last(srt_gcm_authentication_tag_size);
    std::array<std::byte, message.size()> plaintext {};
    auto repeat = decoded.packet.data;
    repeat.retransmitted = true;
    REQUIRE_EQ(
        member.receiver.open(repeat, ciphertext, tag, plaintext), Error::none);
    REQUIRE_EQ(plaintext, message);
    repeat.destination_socket_id ^= 1U;
    REQUIRE_EQ(member.receiver.open(repeat, ciphertext, tag, plaintext),
        Error::authentication_failure);
    REQUIRE(std::all_of(plaintext.begin(), plaintext.end(), [](std::byte b) {
        return b == std::byte {};
    }));
}

TEST(compat_group_encoder_close_preserves_only_external_immutable_owners)
{
    auto encoder =
        Encoder::create(configuration(schedule(CryptoMode::aes_ctr)));
    REQUIRE(encoder);
    auto prepared = encoder->prepare(header(), message);
    REQUIRE(prepared);
    std::weak_ptr<const void> generation = prepared.packet->generation_owner();
    encoder->close();
    encoder->close();
    REQUIRE(encoder->snapshot().closed);
    REQUIRE(!encoder->snapshot().prepared);
    REQUIRE_EQ(encoder->snapshot().cached_generations, 0U);
    REQUIRE(!generation.expired());
    REQUIRE(!encoder->commit(prepared.packet));
    REQUIRE_EQ(encoder->prepare(header(), message).status, Status::closed);
    prepared.packet.reset();
    REQUIRE(generation.expired());
}

namespace {
struct Emission {
    unsigned failures = 2;
    std::vector<std::vector<std::byte>> attempts;
    GroupMulticastEmitter::TimePoint now {};
};
UdpIoResult emit(
    std::span<const std::byte> bytes, IpEndpoint, void* context) noexcept
{
    auto& output = *static_cast<Emission*>(context);
    output.attempts.emplace_back(bytes.begin(), bytes.end());
    if (output.failures != 0U) {
        --output.failures;
        return {.error = Error::would_block};
    }
    return {.bytes_transferred = bytes.size()};
}
GroupMulticastEmitter::TimePoint time_now(void* context) noexcept
{
    return static_cast<Emission*>(context)->now;
}
} // namespace

TEST(
    compat_group_encoder_emitter_pipeline_retains_real_crypto_after_encoder_close)
{
    using namespace std::chrono_literals;
    for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        auto keys = schedule(mode);
        Member member(keys);
        auto encoder = Encoder::create(configuration(keys));
        REQUIRE(encoder);
        auto prepared = encoder->prepare(header(), message);
        REQUIRE(prepared);
        auto expected = member.encode(header(), message);
        std::weak_ptr<const void> generation =
            prepared.packet->generation_owner();
        Emission output;
        auto channel = std::make_shared<DatagramChannel>();
        channel->set_send_hook_for_testing(emit, &output);
        auto emitter =
            GroupMulticastEmitter::create_for_testing({.channel = channel,
                .destination = {.address = {239, 255, 133, 11}, .port = 6000},
                .group_id = group_id,
                .generation = stream_generation,
                .initial_sequence = initial,
                .wire_bytes_per_second = 1'000'000,
                .clock = time_now,
                .clock_context = &output});
        REQUIRE(emitter);
        REQUIRE_EQ(emitter->enqueue(stream_generation, prepared.packet->bytes(),
                       output.now + 1s, prepared.packet->generation_owner()),
            GroupMulticastEmitter::Admission::accepted);
        REQUIRE(encoder->commit(prepared.packet));
        member.decrypt(*prepared.packet, message);
        prepared.packet.reset();
        encoder->close();
        keys.reset();
        REQUIRE(!generation.expired());
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            emitter->pump_for_testing();
            output.now += 1ms;
        }
        REQUIRE_EQ(output.attempts.size(), 3U);
        for (const auto& attempt : output.attempts)
            REQUIRE_EQ(attempt, expected);
        REQUIRE_EQ(emitter->snapshot().submitted_packets, 1U);
        REQUIRE(generation.expired());
    }
}

TEST(
    compat_group_encoder_full_emitter_retry_does_not_reseal_or_advance_preparation)
{
    using namespace std::chrono_literals;
    auto encoder =
        Encoder::create(configuration(schedule(CryptoMode::aes_gcm)));
    REQUIRE(encoder);
    Emission output;
    output.failures = 0;
    auto channel = std::make_shared<DatagramChannel>();
    channel->set_send_hook_for_testing(emit, &output);
    auto emitter =
        GroupMulticastEmitter::create_for_testing({.channel = channel,
            .destination = {.address = {239, 255, 133, 11}, .port = 6000},
            .group_id = group_id,
            .generation = stream_generation,
            .initial_sequence = initial,
            .capacity = 1,
            .wire_bytes_per_second = 1'000'000,
            .clock = time_now,
            .clock_context = &output});
    REQUIRE(emitter);
    auto enqueue = [&](const std::shared_ptr<const Encoder::Packet>& p) {
        return emitter->enqueue(p->stream_generation(), p->bytes(),
            output.now + 1s, p->generation_owner());
    };
    auto first = encoder->prepare(header(), message);
    REQUIRE(first);
    REQUIRE_EQ(
        enqueue(first.packet), GroupMulticastEmitter::Admission::accepted);
    REQUIRE(encoder->commit(first.packet));
    auto second = encoder->prepare(header(initial.next()), message);
    REQUIRE(second);
    REQUIRE_EQ(enqueue(second.packet), GroupMulticastEmitter::Admission::full);
    REQUIRE_EQ(encoder->snapshot().committed_packets, 1U);
    auto same = encoder->prepare(header(initial.next()), message);
    REQUIRE(same);
    REQUIRE_EQ(same.packet, second.packet);
    emitter->pump_for_testing();
    REQUIRE_EQ(
        enqueue(same.packet), GroupMulticastEmitter::Admission::accepted);
    REQUIRE(encoder->commit(same.packet));
    output.now += 1ms;
    emitter->pump_for_testing();
    REQUIRE_EQ(emitter->snapshot().submitted_packets, 2U);
    REQUIRE_EQ(output.attempts.size(), 2U);
}

namespace {
class PartialCtr final : public PayloadCipher {
    Error transform(std::span<const std::byte, aes_block_size>,
        std::span<const std::byte>,
        std::span<std::byte> output) noexcept override
    {
        std::fill(output.begin(), output.end(), std::byte {0xa5});
        return Error::cryptographic_failure;
    }
};
class PartialGcm final : public AuthenticatedPayloadCipher {
    Error seal(std::span<const std::byte>, std::span<const std::byte>,
        std::span<const std::byte>, std::span<std::byte> output,
        std::span<std::byte> tag) noexcept override
    {
        std::fill(output.begin(), output.end(), std::byte {0xa5});
        std::fill(tag.begin(), tag.end(), std::byte {0xa5});
        return Error::cryptographic_failure;
    }
    Error open(std::span<const std::byte>, std::span<const std::byte>,
        std::span<const std::byte>, std::span<const std::byte>,
        std::span<std::byte>) noexcept override
    {
        return Error::unsupported;
    }
};
class FailingProvider final : public CryptoProvider {
public:
    enum class Failure { construction, null_cipher, partial_output };
    Failure failure = Failure::construction;
    bool gcm_supported = true;
    bool scratch_key_erased = false;
    bool partial_datagram_erased = false;
    const std::byte* scratch_key = nullptr;
    Error random_bytes(std::span<std::byte> output) noexcept override
    {
        return delegate.random_bytes(output);
    }
    Error pbkdf2_hmac_sha1(std::span<const std::byte> p,
        std::span<const std::byte> s, std::uint32_t n,
        std::span<std::byte> output) noexcept override
    {
        return delegate.pbkdf2_hmac_sha1(p, s, n, output);
    }
    Error wrap_key(std::span<const std::byte> k, std::span<const std::byte> p,
        std::span<std::byte> output, std::size_t& n) noexcept override
    {
        return delegate.wrap_key(k, p, output, n);
    }
    Error unwrap_key(std::span<const std::byte> k, std::span<const std::byte> c,
        std::span<std::byte> output, std::size_t& n) noexcept override
    {
        return delegate.unwrap_key(k, c, output, n);
    }
    Error make_aes_ctr_cipher(std::span<const std::byte> key,
        std::unique_ptr<PayloadCipher>& output) noexcept override
    {
        scratch_key = key.data();
        output.reset();
        if (failure == Failure::construction)
            return Error::cryptographic_failure;
        if (failure == Failure::partial_output)
            output = std::make_unique<PartialCtr>();
        return Error::none;
    }
    bool supports_aes_gcm() const noexcept override
    {
        return gcm_supported;
    }
    Error make_aes_gcm_cipher(std::span<const std::byte> key,
        std::unique_ptr<AuthenticatedPayloadCipher>& output) noexcept override
    {
        scratch_key = key.data();
        output.reset();
        if (failure == Failure::construction)
            return Error::cryptographic_failure;
        if (failure == Failure::partial_output)
            output = std::make_unique<PartialGcm>();
        return Error::none;
    }
    void secure_erase(std::span<std::byte> bytes) noexcept override
    {
        delegate.secure_erase(bytes);
        const bool zero =
            std::all_of(bytes.begin(), bytes.end(), [](std::byte b) {
                return b == std::byte {};
            });
        if (bytes.data() == scratch_key)
            scratch_key_erased = zero;
        if (bytes.size() == Encoder::maximum_datagram_size)
            partial_datagram_erased = zero;
    }

private:
    CryptoProvider& delegate = default_crypto_provider();
};
} // namespace

TEST(
    compat_group_encoder_crypto_failure_erases_partial_output_and_forbids_nonce_replacement)
{
    for (const auto mode : {CryptoMode::aes_ctr, CryptoMode::aes_gcm}) {
        for (const auto failure : {FailingProvider::Failure::construction,
                 FailingProvider::Failure::null_cipher,
                 FailingProvider::Failure::partial_output}) {
            FailingProvider provider;
            provider.failure = failure;
            auto encoder =
                Encoder::create(configuration(schedule(mode)), provider);
            REQUIRE(encoder);
            const auto failed = encoder->prepare(header(), message);
            REQUIRE_EQ(failed.status, Status::failed);
            REQUIRE_EQ(failed.error, Error::cryptographic_failure);
            REQUIRE(!failed.packet);
            REQUIRE(provider.scratch_key_erased);
            if (failure == FailingProvider::Failure::partial_output)
                REQUIRE(provider.partial_datagram_erased);
            auto changed = message;
            changed[0] ^= std::byte {1};
            REQUIRE_EQ(
                encoder->prepare(header(), changed).status, Status::failed);
            REQUIRE_EQ(encoder->snapshot().committed_packets, 0U);
            REQUIRE_EQ(encoder->snapshot().live_packets, 0U);
            REQUIRE(!encoder->snapshot().prepared);
        }
    }
    FailingProvider unsupported;
    unsupported.gcm_supported = false;
    REQUIRE(!Encoder::create(
        configuration(schedule(CryptoMode::aes_gcm)), unsupported));
}

TEST(compat_group_encoder_cannot_start_behind_live_shared_schedule_frontier)
{
    auto keys = schedule(CryptoMode::aes_ctr);
    REQUIRE_EQ(keys->position_of(initial.advanced(2)), 2U);
    REQUIRE(!Encoder::create(configuration(keys)));
    auto c = configuration(keys);
    c.initial_sequence = initial.advanced(2);
    REQUIRE(Encoder::create(c));
}
