#include "test.hpp"

#include "robotweax/srt/codec.hpp"
#include "compat/file_io.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <streambuf>
#include <string>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

struct CapturedDatagrams {
    std::mutex mutex;
    std::vector<std::vector<std::byte>> values;
};

class FailingWriteBuffer final : public std::streambuf {
protected:
    std::streamsize xsputn(
        const char*, std::streamsize) override
    {
        return 0;
    }

    int_type overflow(int_type) override
    {
        return traits_type::eof();
    }
};

UdpIoResult capture_datagram(
    std::span<const std::byte> bytes,
    Ipv4Endpoint,
    void* context) noexcept
{
    auto& captured = *static_cast<CapturedDatagrams*>(context);
    try {
        std::lock_guard lock(captured.mutex);
        captured.values.emplace_back(bytes.begin(), bytes.end());
        return {.bytes_transferred = bytes.size()};
    } catch (...) {
        return {.error = Error::io_error};
    }
}

void deliver(
    CapturedDatagrams& captured,
    ConnectionRuntime& destination,
    Ipv4Endpoint source)
{
    std::vector<std::vector<std::byte>> datagrams;
    {
        std::lock_guard lock(captured.mutex);
        datagrams.swap(captured.values);
    }
    for (const auto& datagram : datagrams) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        destination.process_packet(decoded.packet, source);
    }
}

std::vector<std::vector<std::byte>> take_datagrams(
    CapturedDatagrams& captured)
{
    std::lock_guard lock(captured.mutex);
    std::vector<std::vector<std::byte>> values;
    values.swap(captured.values);
    return values;
}

std::uint64_t injected_now(void* context) noexcept
{
    return *static_cast<const std::uint64_t*>(context);
}

struct TemporaryFiles {
    TemporaryFiles()
    {
        const auto identifier = std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count();
        const auto directory =
            std::filesystem::temp_directory_path();
        source = directory
            / ("robotweax-srt-source-"
                + std::to_string(identifier));
        destination = directory
            / ("robotweax-srt-destination-"
                + std::to_string(identifier));
        source_api_path = source.string();
        destination_api_path = destination.string();
    }

    ~TemporaryFiles()
    {
        std::error_code ignored;
        (void)std::filesystem::remove(source, ignored);
        (void)std::filesystem::remove(destination, ignored);
    }

    std::filesystem::path source;
    std::filesystem::path destination;
    std::string source_api_path;
    std::string destination_api_path;
};

} // namespace

TEST(compat_file_api_transfers_a_fragment_and_updates_offsets)
{
    TemporaryFiles files;
    const std::array<char, 10> source_bytes{
        'a', 'b', 'c', 'd', 'e',
        'f', 'g', 'h', 'i', 'j'};
    {
        std::ofstream source(
            files.source, std::ios::binary);
        source.write(
            source_bytes.data(),
            static_cast<std::streamsize>(
                source_bytes.size()));
        REQUIRE(source.good());
    }

    const auto sender_channel =
        std::make_shared<DatagramChannel>();
    const auto receiver_channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams sender_output;
    CapturedDatagrams receiver_output;
    sender_channel->set_send_hook_for_testing(
        capture_datagram, &sender_output);
    receiver_channel->set_send_hook_for_testing(
        capture_datagram, &receiver_output);
    const Ipv4Endpoint sender_endpoint{
        .address = {192, 0, 2, 51},
        .port = 17'001,
    };
    const Ipv4Endpoint receiver_endpoint{
        .address = {192, 0, 2, 52},
        .port = 17'002,
    };

    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::transmission_type,
                   static_cast<std::int64_t>(
                       TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size, 2),
        Error::none);
    std::uint64_t sender_now = 100'000;
    std::uint64_t receiver_now = 100'000;
    const auto origin = ConnectionRuntime::Clock::now();
    const auto sender = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration{
            .channel = sender_channel,
            .peer = receiver_endpoint,
            .peer_socket_id = 520,
            .initial_sequence =
                SequenceNumber{SequenceNumber::mask - 1U},
            .flow_window_packets = 256,
            .options = options,
            .origin = origin,
            .now_function = injected_now,
            .now_context = &sender_now,
        });
    const auto receiver = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration{
            .channel = receiver_channel,
            .peer = sender_endpoint,
            .peer_socket_id = 510,
            .initial_sequence =
                SequenceNumber{SequenceNumber::mask - 1U},
            .flow_window_packets = 256,
            .options = options,
            .origin = origin,
            .now_function = injected_now,
            .now_context = &receiver_now,
        });

    SocketRecord sender_record;
    sender_record.state = SRTS_CONNECTED;
    sender_record.public_options.transmission_type =
        SRTT_FILE;
    sender_record.public_options.message_api = false;
    sender_record.public_options.tsbpd_mode = false;
    sender_record.runtime = sender;
    SocketRecord receiver_record;
    receiver_record.state = SRTS_CONNECTED;
    receiver_record.public_options.transmission_type =
        SRTT_FILE;
    receiver_record.public_options.message_api = false;
    receiver_record.public_options.tsbpd_mode = false;
    receiver_record.runtime = receiver;

    std::int64_t send_offset = 2;
    REQUIRE_EQ(send_file(
                   &sender_record,
                   files.source_api_path.c_str(),
                   &send_offset, 5, 3),
        5);
    REQUIRE_EQ(send_offset, 7);
    REQUIRE(!sender->wait_for_send_drain(
        std::chrono::milliseconds{0}));

    for (std::size_t iteration = 0; iteration < 8; ++iteration) {
        sender_now += 10;
        receiver_now += 10;
        (void)sender->poll();
        deliver(sender_output, *receiver, sender_endpoint);
        deliver(receiver_output, *sender, receiver_endpoint);
    }
    REQUIRE(sender->wait_for_send_drain(
        std::chrono::milliseconds{0}));

    std::int64_t receive_offset = 1;
    REQUIRE_EQ(receive_file(
                   &receiver_record,
                   files.destination_api_path.c_str(),
                   &receive_offset, 5, 2),
        5);
    REQUIRE_EQ(receive_offset, 6);

    std::ifstream destination(
        files.destination, std::ios::binary);
    const std::vector<char> received{
        std::istreambuf_iterator<char>{destination},
        std::istreambuf_iterator<char>{}};
    REQUIRE_EQ(received.size(), 6U);
    REQUIRE(std::equal(
        received.begin() + 1,
        received.end(),
        source_bytes.begin() + 2));
}

TEST(compat_file_api_reports_argument_mode_and_file_errors)
{
    TemporaryFiles files;
    {
        std::ofstream source(
            files.source, std::ios::binary);
        source << "abc";
    }

    std::int64_t offset = 0;
    REQUIRE_EQ(send_file(
                   nullptr, files.source_api_path.c_str(),
                   &offset, 1, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE_EQ(send_file(
                   nullptr, nullptr,
                   &offset, 1, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVPARAM);

    const auto channel =
        std::make_shared<DatagramChannel>();
    const auto runtime =
        std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration{
                .channel = channel,
                .peer = {
                    .address = {192, 0, 2, 61},
                    .port = 18'001,
                },
                .peer_socket_id = 610,
                .initial_sequence = SequenceNumber{10},
                .flow_window_packets = 256,
                .origin = ConnectionRuntime::Clock::now(),
            });
    SocketRecord record;
    record.state = SRTS_CONNECTED;
    record.runtime = runtime;

    REQUIRE_EQ(send_file(
                   &record, files.source_api_path.c_str(),
                   &offset, 1, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr),
        SRT_EINVALBUFFERAPI);

    record.public_options.message_api = false;
    record.public_options.tsbpd_mode = false;
    REQUIRE_EQ(send_file(
                   &record, files.source_api_path.c_str(),
                   &offset, 0, 1),
        0);
    REQUIRE_EQ(receive_file(
                   &record, files.destination_api_path.c_str(),
                   &offset, 0, 1),
        0);
    REQUIRE_EQ(send_file(
                   &record, files.source_api_path.c_str(),
                   &offset, 1, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr),
        SRT_EINVALBUFFERAPI);

    record.public_options.transmission_type = SRTT_FILE;
    offset = 4;
    REQUIRE_EQ(send_file(
                   &record, files.source_api_path.c_str(),
                   &offset, 1, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVRDOFF);

    const auto missing_directory =
        files.destination / "missing" / "output";
    const auto missing_directory_api_path =
        missing_directory.string();
    offset = 0;
    REQUIRE_EQ(receive_file(
                   &record,
                   missing_directory_api_path.c_str(),
                   &offset, 1, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EWRPERM);
}

TEST(compat_public_file_api_opens_paths_before_validating_the_socket)
{
    TemporaryFiles files;
    {
        std::ofstream source(
            files.source, std::ios::binary);
        source << "abc";
    }

    std::int64_t offset = 0;
    REQUIRE_EQ(srt_sendfile(
                   SRT_INVALID_SOCK,
                   files.source_api_path.c_str(),
                   &offset, 1, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);

    const auto missing_source =
        files.source / "missing";
    const auto missing_source_api_path =
        missing_source.string();
    REQUIRE_EQ(srt_sendfile(
                   SRT_INVALID_SOCK,
                   missing_source_api_path.c_str(),
                   &offset, 1, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ERDPERM);

    REQUIRE_EQ(srt_recvfile(
                   SRT_INVALID_SOCK,
                   files.destination_api_path.c_str(),
                   &offset, 1, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EINVSOCK);
    REQUIRE(std::filesystem::exists(files.destination));
}

TEST(compat_recvfile_write_failure_signals_peer_error_to_the_sender)
{
    TemporaryFiles files;
    {
        std::ofstream source(files.source, std::ios::binary);
        source << "xy";
    }

    const auto sender_channel =
        std::make_shared<DatagramChannel>();
    const auto receiver_channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams sender_output;
    CapturedDatagrams receiver_output;
    sender_channel->set_send_hook_for_testing(
        capture_datagram, &sender_output);
    receiver_channel->set_send_hook_for_testing(
        capture_datagram, &receiver_output);
    const Ipv4Endpoint sender_endpoint{
        .address = {192, 0, 2, 71},
        .port = 19'001,
    };
    const Ipv4Endpoint receiver_endpoint{
        .address = {192, 0, 2, 72},
        .port = 19'002,
    };

    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::transmission_type,
                   static_cast<std::int64_t>(
                       TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size, 2),
        Error::none);
    std::uint64_t sender_now = 100'000;
    std::uint64_t receiver_now = 100'000;
    const auto origin = ConnectionRuntime::Clock::now();
    const auto sender = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration{
            .channel = sender_channel,
            .peer = receiver_endpoint,
            .peer_socket_id = 720,
            .initial_sequence = SequenceNumber{1'000},
            .flow_window_packets = 256,
            .options = options,
            .origin = origin,
            .now_function = injected_now,
            .now_context = &sender_now,
        });
    const auto receiver = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration{
            .channel = receiver_channel,
            .peer = sender_endpoint,
            .peer_socket_id = 710,
            .initial_sequence = SequenceNumber{1'000},
            .flow_window_packets = 256,
            .options = options,
            .origin = origin,
            .now_function = injected_now,
            .now_context = &receiver_now,
        });

    SocketRecord sender_record;
    sender_record.state = SRTS_CONNECTED;
    sender_record.public_options.transmission_type = SRTT_FILE;
    sender_record.public_options.message_api = false;
    sender_record.public_options.tsbpd_mode = false;
    sender_record.runtime = sender;
    SocketRecord receiver_record;
    receiver_record.state = SRTS_CONNECTED;
    receiver_record.public_options.transmission_type = SRTT_FILE;
    receiver_record.public_options.message_api = false;
    receiver_record.public_options.tsbpd_mode = false;
    receiver_record.runtime = receiver;

    const std::array<std::byte, 2> payload{
        std::byte{'x'}, std::byte{'y'}};
    REQUIRE_EQ(sender->queue_stream(
                   payload, false, -1).status,
        MessageIoStatus::success);
    (void)sender->poll();
    deliver(sender_output, *receiver, sender_endpoint);

    FailingWriteBuffer failed_buffer;
    std::ostream failed_output{&failed_buffer};
    std::int64_t receive_offset = 0;
    REQUIRE_EQ(receive_file_stream(
                   &receiver_record, failed_output,
                   &receive_offset, 2, 2),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EWRPERM);
    REQUIRE_EQ(receive_offset, 0);

    bool found_peer_error = false;
    for (const auto& datagram :
        take_datagrams(receiver_output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        if (decoded.packet.kind == PacketKind::control
            && decoded.packet.control.type
                == ControlType::peer_error) {
            found_peer_error = true;
            REQUIRE_EQ(
                decoded.packet.control.type_specific,
                static_cast<std::uint32_t>(SRT_EFILE));
            REQUIRE_EQ(decoded.packet.payload.size(),
                sizeof(std::uint32_t));
            REQUIRE(std::all_of(
                decoded.packet.payload.begin(),
                decoded.packet.payload.end(),
                [](std::byte value) {
                    return value == std::byte{0};
                }));
        }
        sender->process_packet(
            decoded.packet, receiver_endpoint);
    }
    REQUIRE(found_peer_error);

    std::int64_t send_offset = 0;
    REQUIRE_EQ(send_file(
                   &sender_record,
                   files.source_api_path.c_str(),
                   &send_offset, 1, 1),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_EPEERERR);
    REQUIRE_EQ(send_offset, 0);
}

TEST(compat_async_linger_closes_after_drain_or_deadline)
{
    auto& registry = SocketRegistry::instance();
    const Ipv4Endpoint sender_endpoint{
        .address = {192, 0, 2, 81},
        .port = 20'001,
    };
    const Ipv4Endpoint receiver_endpoint{
        .address = {192, 0, 2, 82},
        .port = 20'002,
    };
    SocketOptions options;
    REQUIRE_EQ(options.set(
                   SocketOption::transmission_type,
                   static_cast<std::int64_t>(
                       TransmissionType::file)),
        Error::none);
    REQUIRE_EQ(options.set(
                   SocketOption::maximum_payload_size, 2),
        Error::none);
    const auto origin = ConnectionRuntime::Clock::now();

    const auto sender_channel =
        std::make_shared<DatagramChannel>();
    const auto receiver_channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams sender_output;
    CapturedDatagrams receiver_output;
    sender_channel->set_send_hook_for_testing(
        capture_datagram, &sender_output);
    receiver_channel->set_send_hook_for_testing(
        capture_datagram, &receiver_output);
    const auto sender = std::make_shared<ConnectionRuntime>(
        ConnectionRuntime::Configuration{
            .channel = sender_channel,
            .peer = receiver_endpoint,
            .peer_socket_id = 820,
            .initial_sequence = SequenceNumber{2'000},
            .flow_window_packets = 256,
            .options = options,
            .origin = origin,
        });
    ConnectionRuntime receiver{{
        .channel = receiver_channel,
        .peer = sender_endpoint,
        .peer_socket_id = 810,
        .initial_sequence = SequenceNumber{2'000},
        .flow_window_packets = 256,
        .options = options,
        .origin = origin,
    }};

    const SRTSOCKET drained_handle = registry.create();
    REQUIRE(drained_handle != SRT_INVALID_SOCK);
    const auto drained_record =
        registry.find(drained_handle);
    REQUIRE(drained_record != nullptr);
    {
        std::lock_guard lock(drained_record->mutex);
        drained_record->state = SRTS_CONNECTED;
        drained_record->channel = sender_channel;
        drained_record->runtime = sender;
        drained_record->public_options.transmission_type =
            SRTT_FILE;
        drained_record->public_options.send_synchronous = false;
        drained_record->public_options.linger_enabled = true;
        drained_record->public_options.linger_seconds = 10;
        REQUIRE_EQ(drained_record->native_options.set_passphrase(
                       "synthetic-linger-secret"),
            Error::none);
    }
    REQUIRE(sender_channel->register_connection(
        static_cast<std::uint32_t>(drained_handle), sender));

    const std::array<std::byte, 2> payload{
        std::byte{'o'}, std::byte{'k'}};
    REQUIRE_EQ(sender->queue_stream(
                   payload, false, -1).status,
        MessageIoStatus::success);
    (void)sender->poll();
    deliver(sender_output, receiver, sender_endpoint);

    registry.close(drained_handle);
    REQUIRE_EQ(registry.state(drained_handle), SRTS_CLOSING);
    REQUIRE(registry.find(drained_handle) == drained_record);
    constexpr char replacement_secret[] = "synthetic-late-secret";
    REQUIRE_EQ(
        srt_setsockflag(drained_handle, SRTO_PASSPHRASE, replacement_secret,
            static_cast<int>(sizeof(replacement_secret) - 1U)),
        SRT_ERROR);
    REQUIRE_EQ(srt_getlasterror(nullptr), SRT_ESCLOSED);
    {
        std::lock_guard lock(drained_record->mutex);
        REQUIRE(drained_record->native_options.passphrase().empty());
    }
    deliver(receiver_output, *sender, receiver_endpoint);
    (void)service_deferred_closes(
        std::chrono::steady_clock::now());
    REQUIRE_EQ(registry.state(drained_handle), SRTS_CLOSED);
    REQUIRE(registry.find(drained_handle) == nullptr);

    bool found_shutdown = false;
    for (const auto& datagram :
        take_datagrams(sender_output)) {
        const auto decoded = decode_packet(datagram);
        REQUIRE(decoded);
        found_shutdown = found_shutdown
            || (decoded.packet.kind == PacketKind::control
                && decoded.packet.control.type
                    == ControlType::shutdown);
    }
    REQUIRE(found_shutdown);

    const auto timeout_channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams timeout_output;
    timeout_channel->set_send_hook_for_testing(
        capture_datagram, &timeout_output);
    const auto timeout_runtime =
        std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration{
                .channel = timeout_channel,
                .peer = receiver_endpoint,
                .peer_socket_id = 830,
                .initial_sequence = SequenceNumber{3'000},
                .flow_window_packets = 256,
                .options = options,
                .origin = origin,
            });
    const SRTSOCKET timeout_handle = registry.create();
    REQUIRE(timeout_handle != SRT_INVALID_SOCK);
    const auto timeout_record =
        registry.find(timeout_handle);
    REQUIRE(timeout_record != nullptr);
    {
        std::lock_guard lock(timeout_record->mutex);
        timeout_record->state = SRTS_CONNECTED;
        timeout_record->channel = timeout_channel;
        timeout_record->runtime = timeout_runtime;
        timeout_record->public_options.transmission_type =
            SRTT_FILE;
        timeout_record->public_options.send_synchronous = false;
        timeout_record->public_options.linger_enabled = true;
        timeout_record->public_options.linger_seconds = 10;
    }
    REQUIRE(timeout_channel->register_connection(
        static_cast<std::uint32_t>(timeout_handle),
        timeout_runtime));
    REQUIRE_EQ(timeout_runtime->queue_stream(
                   payload, false, -1).status,
        MessageIoStatus::success);

    registry.close(timeout_handle);
    REQUIRE_EQ(registry.state(timeout_handle), SRTS_CLOSING);
    REQUIRE_EQ(service_deferred_closes(
                   std::chrono::steady_clock::now()
                       + std::chrono::seconds{11}),
        1U);
    REQUIRE_EQ(registry.state(timeout_handle), SRTS_CLOSED);

    const auto cleanup_channel =
        std::make_shared<DatagramChannel>();
    CapturedDatagrams cleanup_output;
    cleanup_channel->set_send_hook_for_testing(
        capture_datagram, &cleanup_output);
    const auto cleanup_runtime =
        std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration{
                .channel = cleanup_channel,
                .peer = receiver_endpoint,
                .peer_socket_id = 840,
                .initial_sequence = SequenceNumber{4'000},
                .flow_window_packets = 256,
                .options = options,
                .origin = origin,
            });
    const SRTSOCKET cleanup_handle = registry.create();
    REQUIRE(cleanup_handle != SRT_INVALID_SOCK);
    const auto cleanup_record =
        registry.find(cleanup_handle);
    REQUIRE(cleanup_record != nullptr);
    {
        std::lock_guard lock(cleanup_record->mutex);
        cleanup_record->state = SRTS_CONNECTED;
        cleanup_record->channel = cleanup_channel;
        cleanup_record->runtime = cleanup_runtime;
        cleanup_record->public_options.transmission_type =
            SRTT_FILE;
        cleanup_record->public_options.send_synchronous = false;
        cleanup_record->public_options.linger_enabled = true;
        cleanup_record->public_options.linger_seconds = 10;
    }
    REQUIRE(cleanup_channel->register_connection(
        static_cast<std::uint32_t>(cleanup_handle),
        cleanup_runtime));
    REQUIRE_EQ(cleanup_runtime->queue_stream(
                   payload, false, -1).status,
        MessageIoStatus::success);
    registry.close(cleanup_handle);
    REQUIRE_EQ(registry.state(cleanup_handle), SRTS_CLOSING);

    registry.clear();
    {
        std::lock_guard lock(cleanup_record->mutex);
        REQUIRE_EQ(cleanup_record->state, SRTS_CLOSED);
    }
    REQUIRE_EQ(registry.state(cleanup_handle), SRTS_NONEXIST);
}
