// SPDX-License-Identifier: MIT
// Standalone receive-generation diagnostic; no clocks, network or private API.
// Build/run instructions and source identities are in the PR #387 review packet.
// Exit 1 is a real DATA round-trip failure, never a passing qualification.
#include "robotweax/srt/crypto.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>

using namespace robotweax::srt;
namespace {
void check(Error error)
{
    if (error != Error::none)
        throw std::runtime_error("fixture crypto operation failed");
}
struct Packet {
    DataHeader header;
    std::array<std::byte, 32> ciphertext {}, plaintext {};
    std::array<std::byte, srt_gcm_authentication_tag_size> tag {};
};
enum class Scenario { ordered, lagging_frontier, advanced_frontier };
bool run(bool gcm, Scenario scenario)
{
    const CryptoConfiguration configuration {
        .passphrase = "synthetic generation-order diagnostic",
        .mode = gcm ? CryptoMode::aes_gcm : CryptoMode::aes_ctr,
        .enable_aes_gcm = gcm,
        .key_length = 32,
        .refresh_rate_packets = 32,
        .preannouncement_packets = 8};
    CryptoSession sender(configuration), receiver(configuration);
    check(sender.start_initiator());
    check(receiver.accept_key_material(sender.pending_key_material(), false));
    check(sender.acknowledge_key_material(
        receiver.key_material_response(), false));

    Packet delayed;
    unsigned exchanges = 0;
    SequenceNumber sequence {100}, highest_accepted;
    std::uint32_t frontier_at_reuse = 0;
    const auto receive = [&](const Packet& packet) {
        std::array<std::byte, 32> output {};
        const auto error = gcm
            ? receiver.open(
                  packet.header, packet.ciphertext, packet.tag, output)
            : receiver.decrypt(packet.header.encryption_key,
                  packet.header.sequence, packet.ciphertext, output);
        const bool same = error == Error::none && output == packet.plaintext;
        if (same) {
            receiver.note_accepted_receive_sequence(packet.header.sequence);
            if (packet.header.sequence.is_after(highest_accepted))
                highest_accepted = packet.header.sequence;
        }
        return std::pair {error, same};
    };
    // The first successor is announced at position 24. The replacement for
    // the original even selector is announced at 56, before it is used at 64.
    // Hold position 23 across that announcement. The lagging scenario also
    // delays subsequent DATA, so the last accepted sequence remains 122.
    // The second control accepts later odd DATA first, advancing the frontier.
    for (unsigned position = 0; position <= 56;
        ++position, sequence = sequence.next()) {
        check(sender.prepare_rotation());
        if (!sender.pending_key_material().empty()) {
            if (++exchanges == 2)
                frontier_at_reuse = highest_accepted.value();
            check(receiver.accept_key_material(
                sender.pending_key_material(), false));
            check(sender.acknowledge_key_material(
                receiver.key_material_response(), false));
        }
        Packet packet;
        packet.header.sequence = sequence;
        packet.header.message_number = position + 1;
        packet.header.encryption_key = sender.active_sender_key();
        for (unsigned i = 0; i < packet.plaintext.size(); ++i)
            packet.plaintext[i] =
                static_cast<std::byte>((position * 17 + i * 29) & 255U);
        EncryptionKey selector;
        check(gcm ? sender.seal(packet.header, packet.plaintext,
                        packet.ciphertext, packet.tag, selector)
                  : sender.encrypt(sequence, packet.plaintext,
                        packet.ciphertext, selector));
        packet.header.encryption_key = selector;
        check(sender.note_data_packet_sent());
        if (position == 23)
            delayed = packet;
        if (scenario == Scenario::ordered || position < 23
            || (scenario == Scenario::advanced_frontier && position > 23
                && position < 56)) {
            if (!receive(packet).second)
                throw std::runtime_error("control DATA mismatch");
        }
    }
    const auto [error, same] = receive(delayed);
    const char* name = scenario == Scenario::ordered ? "ordered-control"
        : scenario == Scenario::lagging_frontier     ? "lagging-frontier"
                                                 : "advanced-frontier-control";
    std::printf("{\"mode\":\"%s\",\"scenario\":\"%s\","
                "\"delayed_position\":23,\"sequence\":123,"
                "\"last_accepted_before_reuse\":%u,\"km_exchanges\":%u,"
                "\"error\":%u,\"plaintext_matches\":%s}\n",
        gcm ? "gcm" : "ctr", name, frontier_at_reuse, exchanges,
        static_cast<unsigned>(error), same ? "true" : "false");
    return same;
}
}
int main()
{
    try {
        bool all = true;
        for (bool gcm : {false, true})
            for (const auto scenario : {Scenario::ordered,
                     Scenario::lagging_frontier, Scenario::advanced_frontier})
                all = run(gcm, scenario) && all;
        return all ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fixture error: %s\n", error.what());
        return 2;
    }
}
