#include "robotweax/srt/crypto.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using robotweax::srt::CryptoConfiguration;
using robotweax::srt::CryptoSession;
using robotweax::srt::CryptoState;
using robotweax::srt::EncryptionKey;
using robotweax::srt::Error;
using robotweax::srt::KeyMaterialBuffer;
using robotweax::srt::SequenceNumber;

constexpr std::uint32_t refresh_rate_packets = 16;
constexpr std::uint32_t preannouncement_packets = 4;
constexpr std::uint64_t retry_interval_ticks = 3;
constexpr std::uint64_t maximum_delay_ticks = 24;
constexpr std::uint32_t loss_percent = 20;
constexpr std::uint32_t duplicate_percent = 15;
constexpr std::size_t maximum_rotations = 1'000'000;

struct Options {
    std::size_t rotations = 1'000;
    std::uint64_t seed = 0x5eed'c1a7'17U;
};

class DeterministicRandom {
public:
    explicit DeterministicRandom(std::uint64_t seed) noexcept
        : state_(seed == 0U ? 1U : seed)
    {
    }

    [[nodiscard]] std::uint64_t next() noexcept
    {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return state_;
    }

    [[nodiscard]] bool chance(std::uint32_t percent) noexcept
    {
        return next() % 100U < percent;
    }

    [[nodiscard]] std::uint64_t up_to(
        std::uint64_t inclusive_maximum) noexcept
    {
        return next() % (inclusive_maximum + 1U);
    }

private:
    std::uint64_t state_;
};

enum class ControlKind : std::uint8_t {
    request,
    response,
};

struct ControlMessage {
    ControlKind kind = ControlKind::request;
    std::uint64_t delivery_tick = 0;
    std::vector<std::byte> material;
};

struct NetworkStatistics {
    std::uint64_t submitted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t duplicated = 0;
    std::uint64_t delivered = 0;
    std::uint64_t reordered_batches = 0;
};

class FaultyControlNetwork {
public:
    explicit FaultyControlNetwork(std::uint64_t seed)
        : random_(seed)
    {
    }

    void send(ControlKind kind,
        std::span<const std::byte> material,
        std::uint64_t now)
    {
        ++statistics_.submitted;
        if (random_.chance(loss_percent)) {
            ++statistics_.dropped;
            return;
        }
        enqueue(kind, material, now);
        if (random_.chance(duplicate_percent)) {
            enqueue(kind, material, now);
            ++statistics_.duplicated;
        }
    }

    [[nodiscard]] std::vector<ControlMessage> take_due(
        std::uint64_t now)
    {
        std::vector<ControlMessage> due;
        for (std::size_t index = queue_.size(); index > 0U;) {
            --index;
            if (queue_[index].delivery_tick <= now) {
                due.push_back(std::move(queue_[index]));
                queue_.erase(queue_.begin()
                    + static_cast<std::ptrdiff_t>(index));
            }
        }
        if (due.size() > 1U) {
            ++statistics_.reordered_batches;
            for (std::size_t index = due.size() - 1U;
                 index > 0U; --index) {
                const std::size_t replacement =
                    static_cast<std::size_t>(
                        random_.up_to(index));
                std::swap(due[index], due[replacement]);
            }
        }
        statistics_.delivered += due.size();
        return due;
    }

    [[nodiscard]] const NetworkStatistics& statistics() const noexcept
    {
        return statistics_;
    }

private:
    void enqueue(ControlKind kind,
        std::span<const std::byte> material,
        std::uint64_t now)
    {
        queue_.push_back({
            .kind = kind,
            .delivery_tick =
                now + random_.up_to(maximum_delay_ticks),
            .material =
                std::vector<std::byte>{
                    material.begin(), material.end()},
        });
    }

    DeterministicRandom random_;
    std::vector<ControlMessage> queue_;
    NetworkStatistics statistics_{};
};

[[nodiscard]] bool parse_unsigned(
    std::string_view text, std::uint64_t& value) noexcept
{
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

[[nodiscard]] bool parse_options(
    int argc, char** argv, Options& options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if ((argument == "--rotations" || argument == "--seed")
            && index + 1 < argc) {
            std::uint64_t value = 0;
            if (!parse_unsigned(argv[++index], value)
                || value == 0U) {
                return false;
            }
            if (argument == "--rotations") {
                options.rotations =
                    static_cast<std::size_t>(value);
            } else {
                options.seed = value;
            }
            continue;
        }
        return false;
    }
    return options.rotations > 0U
        && options.rotations <= maximum_rotations;
}

void copy_material(KeyMaterialBuffer& destination,
    std::span<const std::byte> source)
{
    std::fill(destination.bytes.begin(),
        destination.bytes.end(), std::byte{0});
    std::copy(source.begin(), source.end(),
        destination.bytes.begin());
    destination.size = source.size();
}

[[nodiscard]] bool require(
    bool condition, std::string_view description)
{
    if (!condition) {
        std::cerr << "crypto soak failure: "
                  << description << '\n';
    }
    return condition;
}

[[nodiscard]] bool require_error(
    Error actual, Error expected,
    std::string_view description)
{
    if (actual != expected) {
        std::cerr << "crypto soak failure: " << description
                  << ": expected "
                  << robotweax::srt::describe(expected)
                  << ", received "
                  << robotweax::srt::describe(actual) << '\n';
        return false;
    }
    return true;
}

[[nodiscard]] bool confirm_directional_keys(
    CryptoSession& first, CryptoSession& second)
{
    for (const auto pair :
        {std::pair {&first, &second}, std::pair {&second, &first}}) {
        if (!require_error(pair.second->accept_key_material(
                               pair.first->pending_key_material(), false),
                Error::none, "install directional key")
            || !require_error(pair.first->acknowledge_key_material(
                                  pair.second->key_material_response(), false),
                Error::none, "confirm directional key")) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool run_corruption_checks()
{
    const CryptoConfiguration configuration{
        .passphrase = "correct horse battery",
        .key_length = 32,
        .refresh_rate_packets = refresh_rate_packets,
        .preannouncement_packets = preannouncement_packets,
    };
    CryptoSession sender{configuration};
    CryptoSession receiver{configuration};
    if (!require_error(sender.start_initiator(), Error::none,
            "start corruption-check sender")
        || !require_error(
            receiver.accept_key_material(sender.pending_key_material(), true),
            Error::none, "install initial receive key")
        || !require_error(sender.acknowledge_key_material(
                              receiver.key_material_response(), true),
            Error::none, "acknowledge initial key")
        || !confirm_directional_keys(sender, receiver)) {
        return false;
    }

    constexpr std::uint32_t announcement_at =
        refresh_rate_packets - preannouncement_packets;
    for (std::uint32_t packet = 0;
         packet < announcement_at; ++packet) {
        if (!require_error(sender.note_data_packet_sent(),
                Error::none,
                "advance to key preannouncement")) {
            return false;
        }
    }
    if (!require_error(sender.prepare_rotation(), Error::none,
            "prepare corruption-check rotation")) {
        return false;
    }

    KeyMaterialBuffer valid_request;
    copy_material(valid_request,
        sender.pending_key_material());
    KeyMaterialBuffer corrupted_header = valid_request;
    corrupted_header.bytes[11] ^= std::byte{1};
    if (!require_error(receiver.accept_key_material(
            corrupted_header.view(), false),
            Error::invalid_key_material,
            "reject corrupted reserved field")
        || !require(receiver.receiver_state()
                == CryptoState::secured,
            "reserved-field corruption downgraded secured receiver")) {
        return false;
    }

    const std::array<std::byte, 7> clear{
        std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3},
        std::byte{4}, std::byte{5}, std::byte{6},
    };
    std::array<std::byte, clear.size()> encrypted{};
    std::array<std::byte, clear.size()> decrypted{};
    EncryptionKey key = EncryptionKey::none;
    if (!require_error(sender.encrypt(
            SequenceNumber{17}, clear, encrypted, key),
            Error::none, "encrypt after malformed KMREQ")
        || !require_error(receiver.decrypt(
                key, SequenceNumber{17}, encrypted, decrypted),
            Error::none, "old receive keys remain intact")
        || !require(decrypted == clear,
            "malformed KMREQ changed installed receive keys")) {
        return false;
    }

    KeyMaterialBuffer corrupted_wrap = valid_request;
    corrupted_wrap.bytes[
        corrupted_wrap.size - 1U] ^= std::byte{1};
    if (!require_error(receiver.accept_key_material(
            corrupted_wrap.view(), false),
            Error::cryptographic_failure,
            "reject corrupted RFC 3394 payload")
        || !require(receiver.receiver_state()
                == CryptoState::secured,
            "wrapped-key corruption downgraded secured receiver")
        || !require_error(receiver.accept_key_material(
                valid_request.view(), false),
            Error::none, "recover with valid key material")) {
        return false;
    }

    KeyMaterialBuffer valid_response;
    copy_material(valid_response,
        receiver.key_material_response());
    KeyMaterialBuffer corrupted_response = valid_response;
    corrupted_response.bytes[
        corrupted_response.size - 1U] ^= std::byte{1};
    if (!require_error(sender.acknowledge_key_material(
            corrupted_response.view(), false),
            Error::invalid_key_material,
            "reject corrupted KMRSP")
        || !require(sender.sender_state()
                == CryptoState::securing,
            "KMRSP corruption changed protected sender state")
        || !require_error(sender.acknowledge_key_material(
                valid_response.view(), false),
            Error::none, "acknowledge valid recovery")) {
        return false;
    }
    return true;
}

struct SoakStatistics {
    std::uint64_t ticks = 0;
    std::uint64_t packets = 0;
    std::uint64_t rotations = 0;
    std::uint64_t boundary_stalls = 0;
    std::uint64_t sequence_wraps = 0;
};

[[nodiscard]] bool run_soak(
    const Options& options, SoakStatistics& statistics,
    NetworkStatistics& network_statistics)
{
    const CryptoConfiguration configuration{
        .passphrase = "correct horse battery",
        .key_length = 32,
        .refresh_rate_packets = refresh_rate_packets,
        .preannouncement_packets = preannouncement_packets,
    };
    CryptoSession sender{configuration};
    CryptoSession receiver{configuration};
    if (!require_error(sender.start_initiator(), Error::none, "start sender")
        || !require_error(
            receiver.accept_key_material(sender.pending_key_material(), true),
            Error::none, "install initial key")
        || !require_error(sender.acknowledge_key_material(
                              receiver.key_material_response(), true),
            Error::none, "acknowledge initial key")
        || !confirm_directional_keys(sender, receiver)) {
        return false;
    }

    FaultyControlNetwork network{options.seed};
    KeyMaterialBuffer last_request;
    std::uint64_t next_retry_tick = 0;
    SequenceNumber sequence{SequenceNumber::mask - 127U};
    EncryptionKey previous_key = sender.active_sender_key();
    std::array<std::byte, 1'316> clear{};
    std::array<std::byte, clear.size()> encrypted{};
    std::array<std::byte, clear.size()> decrypted{};
    const std::uint64_t maximum_ticks =
        static_cast<std::uint64_t>(options.rotations)
            * refresh_rate_packets * 200U
        + 10'000U;

    for (std::uint64_t tick = 0;
         statistics.rotations < options.rotations
            && tick < maximum_ticks;
         ++tick) {
        statistics.ticks = tick + 1U;
        if (!require_error(sender.prepare_rotation(),
                Error::none, "prepare rotation")) {
            return false;
        }

        const auto pending = sender.pending_key_material();
        const bool new_request =
            !pending.empty()
            && (last_request.size != pending.size()
                || !std::equal(pending.begin(), pending.end(),
                    last_request.bytes.begin()));
        if (!pending.empty()
            && (new_request || tick >= next_retry_tick)) {
            network.send(
                ControlKind::request, pending, tick);
            copy_material(last_request, pending);
            next_retry_tick = tick + retry_interval_ticks;
        } else if (pending.empty()) {
            last_request.size = 0;
        }

        for (auto& control : network.take_due(tick)) {
            if (control.kind == ControlKind::request) {
                const Error accepted =
                    receiver.accept_key_material(
                        control.material, false);
                if (!require_error(accepted, Error::none,
                        "accept delayed KMREQ")) {
                    return false;
                }
                network.send(ControlKind::response,
                    receiver.key_material_response(), tick);
            } else {
                const Error acknowledged =
                    sender.acknowledge_key_material(
                        control.material, false);
                if (!require_error(acknowledged, Error::none,
                        "accept delayed KMRSP")) {
                    return false;
                }
            }
        }

        if (!sender.ready_to_send_data()) {
            ++statistics.boundary_stalls;
            continue;
        }

        for (std::size_t index = 0;
             index < clear.size(); ++index) {
            clear[index] = static_cast<std::byte>(
                (statistics.packets + index) & 0xffU);
        }
        EncryptionKey packet_key = EncryptionKey::none;
        if (!require_error(sender.encrypt(sequence,
                clear, encrypted, packet_key),
                Error::none, "encrypt soak packet")
            || !require_error(receiver.decrypt(packet_key,
                    sequence, encrypted, decrypted),
                Error::none, "decrypt soak packet")
            || !require(decrypted == clear,
                "decrypted soak payload mismatch")
            || !require_error(sender.note_data_packet_sent(),
                Error::none, "advance packet key lifetime")) {
            return false;
        }

        ++statistics.packets;
        const SequenceNumber next = sequence.next();
        if (next.value() < sequence.value()) {
            ++statistics.sequence_wraps;
        }
        sequence = next;
        const EncryptionKey active = sender.active_sender_key();
        if (active != previous_key) {
            ++statistics.rotations;
            previous_key = active;
        }
    }

    network_statistics = network.statistics();
    return require(statistics.rotations == options.rotations,
            "rotation target was not reached")
        && require(statistics.sequence_wraps > 0U,
            "sequence rollover was not exercised")
        && require(statistics.boundary_stalls > 0U,
            "fault schedule did not exercise a boundary stall")
        && require(network_statistics.dropped > 0U,
            "fault schedule did not drop a control packet")
        && require(network_statistics.duplicated > 0U,
            "fault schedule did not duplicate a control packet")
        && require(network_statistics.reordered_batches > 0U,
            "fault schedule did not reorder a control batch")
        && require(sender.sender_state() == CryptoState::secured,
            "sender did not finish secured")
        && require(receiver.receiver_state() == CryptoState::secured,
            "receiver did not finish secured");
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr
            << "usage: robotweax_srt_crypto_soak "
               "[--rotations N] [--seed N]\n";
        return 2;
    }
    if (!run_corruption_checks()) {
        return 1;
    }

    SoakStatistics statistics;
    NetworkStatistics network;
    if (!run_soak(options, statistics, network)) {
        return 1;
    }
    std::cout
        << "crypto soak passed"
        << ": seed=" << options.seed
        << " rotations=" << statistics.rotations
        << " packets=" << statistics.packets
        << " ticks=" << statistics.ticks
        << " stalls=" << statistics.boundary_stalls
        << " sequence_wraps=" << statistics.sequence_wraps
        << " control_submitted=" << network.submitted
        << " dropped=" << network.dropped
        << " duplicated=" << network.duplicated
        << " delivered=" << network.delivered
        << " reordered_batches=" << network.reordered_batches
        << '\n';
    return 0;
}
