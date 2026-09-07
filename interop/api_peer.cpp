// Black-box interoperability peer using only Haivision's public srt.h surface.
//
// The same source is linked once against Robotweax SRT and once against the
// pinned Haivision library.  This keeps test behavior identical on both sides.

#include "srt.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

enum class Role {
    listener,
    caller,
    rendezvous_sender,
    rendezvous_receiver,
};

struct Configuration {
    Role role = Role::caller;
    std::string host;
    std::uint16_t port = 0;
    std::string local_host;
    std::uint16_t local_port = 0;
    std::string input_path;
    std::string output_path;
    std::uint64_t byte_count = 0;
    int timeout_milliseconds = 30'000;
    int peer_idle_timeout_milliseconds = -1;
    int chunk_size = 1'200;
    int receive_size = 65'536;
    int send_buffer_bytes = -1;
    int flow_window_packets = -1;
    int maximum_segment_size = -1;
    int maximum_payload_size = -1;
    int expected_maximum_payload_size = -1;
    int maximum_reorder_tolerance_packets = -1;
    int ip_time_to_live = -1;
    int ip_type_of_service = -1;
    SRT_TRANSTYPE transport = SRTT_LIVE;
    int message_api_override = -1;
    std::string congestion_controller;
    std::string passphrase_environment;
    int pbkeylen = 0;
#ifdef ENABLE_AEAD_API_PREVIEW
    int crypto_mode = -1;
    int expected_crypto_mode = -1;
#endif
    std::string packet_filter;
    std::string stream_id;
    std::string expected_stream_id;
    std::string listen_callback_stream_id;
    std::string listen_callback_passphrase_environment;
    int listen_callback_rejection_reason = -1;
    int expected_rejection_reason = -1;
    int expected_connect_error = SRT_SUCCESS;
    int latency_milliseconds = -1;
    int too_late_packet_drop = -1;
    std::int64_t input_bandwidth = -1;
    std::int64_t maximum_bandwidth = -1;
    std::int64_t second_maximum_bandwidth = -1;
    int key_refresh_rate = -1;
    int key_preannouncement = -1;
    int shutdown_grace_milliseconds = 0;
    int sender_start_delay_milliseconds = 0;
    bool source_pacing = false;
    bool explicit_source_time = false;
    bool measure_timing = false;
    bool expect_eof = false;
    bool expect_peer_shutdown = false;
    bool expect_peer_idle_timeout = false;
    bool file_api = false;
    bool file_size_to_eof = false;
    bool expect_peer_error = false;
    bool expect_file_write_error = false;
    bool expect_injected_peer_error = false;
    bool require_missing_injected_peer_error = false;
    bool allow_missing_injected_peer_error = false;
    bool connect_callback = false;
    bool require_connect_callback = false;
    bool receive_contract_sender = false;
    bool receive_contract_receiver = false;
    int data_sender_override = -1;
    std::int32_t minimum_initial_sequence = -1;
    int initial_sequence_search_limit = 100'000;
};

bool is_rendezvous(Role role) noexcept
{
    return role == Role::rendezvous_sender
        || role == Role::rendezvous_receiver;
}

bool is_sender(Role role) noexcept
{
    return role == Role::caller
        || role == Role::rendezvous_sender;
}

bool is_sender(const Configuration& configuration) noexcept
{
    return configuration.data_sender_override >= 0
        ? configuration.data_sender_override != 0
        : is_sender(configuration.role);
}

bool uses_message_api(const Configuration& configuration) noexcept
{
    return configuration.message_api_override >= 0
        ? configuration.message_api_override != 0
        : configuration.transport == SRTT_LIVE;
}

const char* role_name(Role role) noexcept
{
    switch (role) {
    case Role::listener: return "listener";
    case Role::caller: return "caller";
    case Role::rendezvous_sender: return "rendezvous-sender";
    case Role::rendezvous_receiver: return "rendezvous-receiver";
    }
    return "unknown";
}

struct SocketHandle {
    SRTSOCKET value = SRT_INVALID_SOCK;

    SocketHandle() = default;
    explicit SocketHandle(SRTSOCKET socket) : value(socket) {}
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    ~SocketHandle()
    {
        if (value != SRT_INVALID_SOCK) {
            (void)srt_close(value);
        }
    }
};

struct StatisticsSnapshot {
    bool available = false;
    SRT_TRACEBSTATS values{};
};

struct ReadinessOptionSnapshot {
    bool available = false;
    std::int32_t events = 0;
    std::int32_t unacknowledged_send_packets = 0;
    std::int32_t available_receive_packets = 0;
};

struct NetworkOptionSnapshot {
    bool available = false;
    std::int32_t ip_time_to_live = 0;
    std::int32_t ip_type_of_service = 0;
};

struct VersionTimingOptionSnapshot {
    bool available = false;
    bool drift_tracer = false;
    std::int64_t minimum_input_bandwidth = 0;
    std::int32_t minimum_peer_version = 0;
    std::int64_t connection_time = -1;
    std::int64_t current_time = -1;
};

template <class Integer>
bool parse_integer(std::string_view text, Integer& value)
{
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size();
}

bool take_value(int& index, int argc, char** argv, std::string_view& value)
{
    if (index + 1 >= argc) {
        return false;
    }
    ++index;
    value = argv[index];
    return true;
}

bool parse_arguments(int argc, char** argv, Configuration& configuration)
{
    if (argc < 2) {
        return false;
    }
    const std::string_view role{argv[1]};
    if (role == "listener") {
        configuration.role = Role::listener;
        configuration.host = "0.0.0.0";
    } else if (role == "caller") {
        configuration.role = Role::caller;
    } else if (role == "rendezvous-sender") {
        configuration.role = Role::rendezvous_sender;
        configuration.local_host = "0.0.0.0";
    } else if (role == "rendezvous-receiver") {
        configuration.role = Role::rendezvous_receiver;
        configuration.local_host = "0.0.0.0";
    } else {
        return false;
    }

    for (int index = 2; index < argc; ++index) {
        const std::string_view option{argv[index]};
        std::string_view value;
        if (option == "--host" && take_value(index, argc, argv, value)) {
            configuration.host = value;
        } else if (option == "--local-host"
            && take_value(index, argc, argv, value)) {
            configuration.local_host = value;
        } else if (option == "--local-port"
            && take_value(index, argc, argv, value)) {
            unsigned parsed = 0;
            if (!parse_integer(value, parsed) || parsed == 0U
                || parsed > 65'535U) {
                return false;
            }
            configuration.local_port =
                static_cast<std::uint16_t>(parsed);
        } else if (option == "--port"
            && take_value(index, argc, argv, value)) {
            unsigned parsed = 0;
            if (!parse_integer(value, parsed) || parsed == 0U
                || parsed > 65'535U) {
                return false;
            }
            configuration.port = static_cast<std::uint16_t>(parsed);
        } else if (option == "--input"
            && take_value(index, argc, argv, value)) {
            configuration.input_path = value;
        } else if (option == "--output"
            && take_value(index, argc, argv, value)) {
            configuration.output_path = value;
        } else if (option == "--bytes"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.byte_count)
                || configuration.byte_count == 0U) {
                return false;
            }
        } else if (option == "--timeout-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.timeout_milliseconds)
                || configuration.timeout_milliseconds <= 0) {
                return false;
            }
        } else if (option == "--peer-idle-timeout-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value, configuration.peer_idle_timeout_milliseconds)
                || configuration.peer_idle_timeout_milliseconds <= 0) {
                return false;
            }
        } else if (option == "--chunk-size"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.chunk_size)
                || configuration.chunk_size <= 0
                || configuration.chunk_size > 65'536) {
                return false;
            }
        } else if (option == "--receive-size"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.receive_size)
                || configuration.receive_size <= 0
                || configuration.receive_size > 65'536) {
                return false;
            }
        } else if (option == "--flow-window"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.flow_window_packets)
                || configuration.flow_window_packets < 32) {
                return false;
            }
        } else if (option == "--send-buffer"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.send_buffer_bytes)
                || configuration.send_buffer_bytes <= 0) {
                return false;
            }
        } else if (option == "--mss" && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.maximum_segment_size)
                || configuration.maximum_segment_size < 76
                || configuration.maximum_segment_size > 1'500) {
                return false;
            }
        } else if (option == "--ipttl"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.ip_time_to_live)
                || configuration.ip_time_to_live < 1
                || configuration.ip_time_to_live > 255) {
                return false;
            }
        } else if (option == "--iptos"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.ip_type_of_service)
                || configuration.ip_type_of_service < 0
                || configuration.ip_type_of_service > 255) {
                return false;
            }
        } else if (option == "--payload-size"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.maximum_payload_size)
                || configuration.maximum_payload_size <= 0
                || configuration.maximum_payload_size
                    > SRT_LIVE_MAX_PLSIZE) {
                return false;
            }
        } else if (option == "--expect-payload-size"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value, configuration.expected_maximum_payload_size)
                || configuration.expected_maximum_payload_size <= 0
                || configuration.expected_maximum_payload_size
                    > SRT_LIVE_MAX_PLSIZE) {
                return false;
            }
        } else if (option == "--loss-max-ttl"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value,
                    configuration.maximum_reorder_tolerance_packets)
                || configuration.maximum_reorder_tolerance_packets < 0) {
                return false;
            }
        } else if (option == "--transport"
            && take_value(index, argc, argv, value)) {
            if (value == "live") {
                configuration.transport = SRTT_LIVE;
            } else if (value == "file") {
                configuration.transport = SRTT_FILE;
            } else {
                return false;
            }
        } else if (option == "--message-api"
            && take_value(index, argc, argv, value)) {
            if (value == "true") {
                configuration.message_api_override = 1;
            } else if (value == "false") {
                configuration.message_api_override = 0;
            } else {
                return false;
            }
        } else if (option == "--data-role"
            && take_value(index, argc, argv, value)) {
            if (value == "sender") {
                configuration.data_sender_override = 1;
            } else if (value == "receiver") {
                configuration.data_sender_override = 0;
            } else {
                return false;
            }
        } else if (option == "--congestion"
            && take_value(index, argc, argv, value)) {
            if (value != "live" && value != "file") {
                return false;
            }
            configuration.congestion_controller = value;
        } else if (option == "--passphrase-env"
            && take_value(index, argc, argv, value)) {
            configuration.passphrase_environment = value;
        } else if (option == "--pbkeylen"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.pbkeylen)
                || (configuration.pbkeylen != 16
                    && configuration.pbkeylen != 24
                    && configuration.pbkeylen != 32)) {
                return false;
            }
#ifdef ENABLE_AEAD_API_PREVIEW
        } else if (option == "--crypto-mode"
            && take_value(index, argc, argv, value)) {
            if (value == "auto") {
                configuration.crypto_mode = 0;
            } else if (value == "ctr") {
                configuration.crypto_mode = 1;
            } else if (value == "gcm") {
                configuration.crypto_mode = 2;
            } else {
                return false;
            }
        } else if (option == "--expect-crypto-mode"
            && take_value(index, argc, argv, value)) {
            if (value == "ctr") {
                configuration.expected_crypto_mode = 1;
            } else if (value == "gcm") {
                configuration.expected_crypto_mode = 2;
            } else {
                return false;
            }
#endif
        } else if (option == "--packet-filter"
            && take_value(index, argc, argv, value)) {
            configuration.packet_filter = value;
        } else if (option == "--stream-id"
            && take_value(index, argc, argv, value)) {
            if (value.empty() || value.size() > 512U) {
                return false;
            }
            configuration.stream_id = value;
        } else if (option == "--expect-stream-id"
            && take_value(index, argc, argv, value)) {
            if (value.empty() || value.size() > 512U) {
                return false;
            }
            configuration.expected_stream_id = value;
        } else if (option == "--listen-callback-stream-id"
            && take_value(index, argc, argv, value)) {
            if (value.empty() || value.size() > 512U) {
                return false;
            }
            configuration.listen_callback_stream_id = value;
        } else if (option == "--listen-callback-passphrase-env"
            && take_value(index, argc, argv, value)) {
            if (value.empty()) {
                return false;
            }
            configuration.listen_callback_passphrase_environment = value;
        } else if (option == "--listen-callback-reject"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value,
                    configuration.listen_callback_rejection_reason)
                || configuration.listen_callback_rejection_reason
                    < SRT_REJC_PREDEFINED) {
                return false;
            }
        } else if (option == "--expect-reject"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value, configuration.expected_rejection_reason)
                || configuration.expected_rejection_reason
                    < SRT_REJC_PREDEFINED) {
                return false;
            }
        } else if (option == "--connect-callback") {
            configuration.connect_callback = true;
        } else if (option == "--require-connect-callback") {
            configuration.require_connect_callback = true;
        } else if (option == "--receive-contract-sender") {
            configuration.receive_contract_sender = true;
        } else if (option == "--receive-contract-receiver") {
            configuration.receive_contract_receiver = true;
        } else if (option == "--expect-connect-error"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value, configuration.expected_connect_error)
                || configuration.expected_connect_error <= SRT_SUCCESS) {
                return false;
            }
        } else if (option == "--latency-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.latency_milliseconds)
                || configuration.latency_milliseconds < 0) {
                return false;
            }
        } else if (option == "--tlpktdrop"
            && take_value(index, argc, argv, value)) {
            if (value == "on") {
                configuration.too_late_packet_drop = 1;
            } else if (value == "off") {
                configuration.too_late_packet_drop = 0;
            } else {
                return false;
            }
        } else if (option == "--input-bw"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.input_bandwidth)
                || configuration.input_bandwidth < 0) {
                return false;
            }
        } else if (option == "--max-bw"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.maximum_bandwidth)
                || configuration.maximum_bandwidth < -1) {
                return false;
            }
        } else if (option == "--second-max-bw"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value, configuration.second_maximum_bandwidth)
                || configuration.second_maximum_bandwidth < 0) {
                return false;
            }
        } else if (option == "--km-refresh-rate"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.key_refresh_rate)
                || configuration.key_refresh_rate <= 0) {
                return false;
            }
        } else if (option == "--km-preannounce"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.key_preannouncement)
                || configuration.key_preannouncement <= 0) {
                return false;
            }
        } else if (option == "--shutdown-grace-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value, configuration.shutdown_grace_milliseconds)
                || configuration.shutdown_grace_milliseconds < 0) {
                return false;
            }
        } else if (option == "--sender-start-delay-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value, configuration.sender_start_delay_milliseconds)
                || configuration.sender_start_delay_milliseconds < 0) {
                return false;
            }
        } else if (option == "--source-pacing") {
            configuration.source_pacing = true;
        } else if (option == "--explicit-source-time") {
            configuration.explicit_source_time = true;
        } else if (option == "--measure-timing") {
            configuration.measure_timing = true;
        } else if (option == "--expect-eof") {
            configuration.expect_eof = true;
        } else if (option == "--expect-peer-shutdown") {
            configuration.expect_peer_shutdown = true;
        } else if (option == "--expect-peer-idle-timeout") {
            configuration.expect_peer_idle_timeout = true;
        } else if (option == "--file-api") {
            configuration.file_api = true;
        } else if (option == "--file-size-to-eof") {
            configuration.file_size_to_eof = true;
        } else if (option == "--expect-peer-error") {
            configuration.expect_peer_error = true;
        } else if (option == "--expect-file-write-error") {
            configuration.expect_file_write_error = true;
        } else if (option == "--expect-injected-peer-error") {
            configuration.expect_injected_peer_error = true;
        } else if (option == "--require-missing-injected-peer-error") {
            configuration.require_missing_injected_peer_error = true;
        } else if (option == "--allow-missing-injected-peer-error") {
            configuration.allow_missing_injected_peer_error = true;
        } else if (option == "--minimum-isn"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value, configuration.minimum_initial_sequence)
                || configuration.minimum_initial_sequence < 0
                || configuration.minimum_initial_sequence
                    > 0x7fff'ffff) {
                return false;
            }
        } else if (option == "--isn-search-limit"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value, configuration.initial_sequence_search_limit)
                || configuration.initial_sequence_search_limit <= 0
                || configuration.initial_sequence_search_limit
                    > 1'000'000) {
                return false;
            }
        } else {
            return false;
        }
    }

    return !configuration.host.empty() && configuration.port != 0
        && configuration.byte_count != 0
        && (!is_rendezvous(configuration.role)
            || (!configuration.local_host.empty()
                && configuration.local_port != 0))
        && (configuration.data_sender_override < 0
            || !is_rendezvous(configuration.role))
        && (configuration.minimum_initial_sequence < 0
            || configuration.role == Role::caller)
        && (configuration.listen_callback_stream_id.empty()
                ? configuration.listen_callback_rejection_reason < 0
                    && configuration.listen_callback_passphrase_environment
                        .empty()
                : configuration.role == Role::listener)
        && (configuration.expected_rejection_reason < 0
            || configuration.role == Role::caller)
        && (!configuration.connect_callback
            || configuration.role == Role::caller)
        && (!configuration.require_connect_callback
            || configuration.connect_callback)
        && (!configuration.receive_contract_sender
            || (is_sender(configuration)
                && !configuration.receive_contract_receiver
                && !is_rendezvous(configuration.role)
                && !configuration.file_api))
        && (!configuration.receive_contract_receiver
            || (!is_sender(configuration)
                && !configuration.receive_contract_sender
                && !is_rendezvous(configuration.role) && !configuration.file_api
                && (uses_message_api(configuration)
                        ? configuration.expect_peer_shutdown
                        : configuration.expect_eof)))
        && (configuration.expected_connect_error == SRT_SUCCESS
            || configuration.connect_callback)
        && (configuration.second_maximum_bandwidth < 0
            || (is_sender(configuration) && configuration.maximum_bandwidth >= 0
                && configuration.byte_count % 2U == 0U
                && (configuration.byte_count / 2U)
                        % static_cast<std::uint64_t>(configuration.chunk_size)
                    == 0U))
        && (!configuration.expect_eof
            || (!is_sender(configuration)
                && configuration.transport == SRTT_FILE))
        && (!configuration.expect_peer_shutdown
            || (!is_sender(configuration)
                && configuration.transport == SRTT_LIVE
                && !configuration.expect_eof
                && !configuration.expect_peer_idle_timeout))
        && (!configuration.expect_peer_idle_timeout
            || (!is_sender(configuration)
                && configuration.transport == SRTT_LIVE
                && !configuration.expect_eof
                && !configuration.expect_peer_shutdown
                && configuration.peer_idle_timeout_milliseconds > 0))
        && (!configuration.file_api || configuration.transport == SRTT_FILE)
        && (!configuration.file_size_to_eof
            || (configuration.file_api && is_sender(configuration)))
        && (!configuration.expect_peer_error
            || (configuration.file_api && is_sender(configuration)))
        && (!configuration.expect_file_write_error
            || (configuration.file_api && !is_sender(configuration)))
        && (!configuration.expect_injected_peer_error
            || (!configuration.file_api && is_sender(configuration)))
        && (!configuration.require_missing_injected_peer_error
            || configuration.expect_injected_peer_error)
        && (!configuration.allow_missing_injected_peer_error
            || (configuration.expect_injected_peer_error
                && !configuration.require_missing_injected_peer_error))
        && (!configuration.source_pacing
            || (is_sender(configuration) && configuration.transport == SRTT_LIVE
                && configuration.input_bandwidth > 0))
        && (configuration.sender_start_delay_milliseconds == 0
            || is_sender(configuration))
        && (!configuration.explicit_source_time
            || (is_sender(configuration) && uses_message_api(configuration)
                && configuration.input_bandwidth > 0))
        && (!configuration.measure_timing
            || (configuration.transport == SRTT_LIVE
                && uses_message_api(configuration)
                && (is_sender(configuration) ? configuration.source_pacing
                            && configuration.explicit_source_time
                            && configuration.input_bandwidth > 0
                                             : true)))
        && (configuration.too_late_packet_drop < 0
            || configuration.transport == SRTT_LIVE)
        && (is_sender(configuration) ? !configuration.input_path.empty()
                                     : !configuration.output_path.empty());
}

template <class Value>
bool set_option(SRTSOCKET socket, SRT_SOCKOPT option, const Value& value)
{
    if (srt_setsockflag(socket, option, &value, sizeof(value)) == SRT_ERROR) {
        std::cerr << "setsockflag " << static_cast<int>(option)
                  << " failed: " << srt_getlasterror_str() << '\n';
        return false;
    }
    return true;
}

bool set_string_option(
    SRTSOCKET socket, SRT_SOCKOPT option, std::string_view value)
{
    if (value.size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max())) {
        return false;
    }
    if (srt_setsockflag(socket, option, value.data(),
            static_cast<int>(value.size())) == SRT_ERROR) {
        std::cerr << "setsockflag " << static_cast<int>(option)
                  << " failed: " << srt_getlasterror_str() << '\n';
        return false;
    }
    return true;
}

bool configure_socket(SRTSOCKET socket, const Configuration& configuration)
{
    const bool data_sender = is_sender(configuration);
    if (!set_option(socket, SRTO_TRANSTYPE, configuration.transport)
        || (configuration.message_api_override >= 0
            && !set_option(
                socket, SRTO_MESSAGEAPI, configuration.message_api_override))
        || !set_option(socket, SRTO_SENDER, data_sender)
        || !set_option(
            socket, SRTO_SNDTIMEO, configuration.timeout_milliseconds)
        || !set_option(
            socket, SRTO_RCVTIMEO, configuration.timeout_milliseconds)) {
        return false;
    }
    if (!configuration.congestion_controller.empty()
        && !set_string_option(socket, SRTO_CONGESTION,
            configuration.congestion_controller)) {
        return false;
    }
    if (configuration.flow_window_packets >= 0
        && !set_option(
            socket, SRTO_FC, configuration.flow_window_packets)) {
        return false;
    }
    if (configuration.send_buffer_bytes > 0
        && !set_option(
            socket, SRTO_SNDBUF, configuration.send_buffer_bytes)) {
        return false;
    }
    if (configuration.maximum_segment_size >= 0
        && !set_option(
            socket, SRTO_MSS, configuration.maximum_segment_size)) {
        return false;
    }
    if (configuration.ip_time_to_live >= 0
        && !set_option(
            socket, SRTO_IPTTL, configuration.ip_time_to_live)) {
        return false;
    }
    if (configuration.ip_type_of_service >= 0
        && !set_option(
            socket, SRTO_IPTOS, configuration.ip_type_of_service)) {
        return false;
    }
    if (configuration.maximum_payload_size >= 0
        && !set_option(socket, SRTO_PAYLOADSIZE,
            configuration.maximum_payload_size)) {
        return false;
    }
    if (configuration.maximum_reorder_tolerance_packets >= 0
        && !set_option(
            socket,
            SRTO_LOSSMAXTTL,
            configuration.maximum_reorder_tolerance_packets)) {
        return false;
    }
    if (configuration.peer_idle_timeout_milliseconds > 0
        && !set_option(socket, SRTO_PEERIDLETIMEO,
            configuration.peer_idle_timeout_milliseconds)) {
        return false;
    }
    if (!configuration.passphrase_environment.empty()) {
        const char* passphrase =
            std::getenv(configuration.passphrase_environment.c_str());
        if (passphrase == nullptr || *passphrase == '\0') {
            std::cerr << "passphrase environment variable is unset\n";
            return false;
        }
        if (!set_string_option(socket, SRTO_PASSPHRASE, passphrase)) {
            return false;
        }
    }
    if (configuration.pbkeylen != 0
        && !set_option(socket, SRTO_PBKEYLEN, configuration.pbkeylen)) {
        return false;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    if (configuration.crypto_mode >= 0
        && !set_option(socket, SRTO_CRYPTOMODE, configuration.crypto_mode)) {
        return false;
    }
#endif
    if (!configuration.packet_filter.empty()
        && !set_string_option(
            socket, SRTO_PACKETFILTER, configuration.packet_filter)) {
        return false;
    }
    if (!configuration.stream_id.empty()
        && !set_string_option(
            socket, SRTO_STREAMID, configuration.stream_id)) {
        return false;
    }
    if (configuration.latency_milliseconds >= 0
        && !set_option(
            socket, SRTO_LATENCY, configuration.latency_milliseconds)) {
        return false;
    }
    if (configuration.too_late_packet_drop >= 0) {
        const bool enabled = configuration.too_late_packet_drop != 0;
        if (!set_option(socket, SRTO_TLPKTDROP, enabled)) {
            return false;
        }
    }
    if (configuration.input_bandwidth >= 0
        && !set_option(socket, SRTO_INPUTBW, configuration.input_bandwidth)) {
        return false;
    }
    if (configuration.maximum_bandwidth >= -1
        && !set_option(socket, SRTO_MAXBW, configuration.maximum_bandwidth)) {
        return false;
    }
    if (configuration.key_refresh_rate > 0
        && !set_option(
            socket, SRTO_KMREFRESHRATE, configuration.key_refresh_rate)) {
        return false;
    }
    if (configuration.key_preannouncement > 0
        && !set_option(
            socket, SRTO_KMPREANNOUNCE, configuration.key_preannouncement)) {
        return false;
    }
    return true;
}

bool verify_expected_stream_id(
    SRTSOCKET socket, const Configuration& configuration)
{
    if (configuration.expected_stream_id.empty()) {
        return true;
    }
    std::array<char, 513> observed{};
    int observed_size = static_cast<int>(observed.size());
    if (srt_getsockflag(socket, SRTO_STREAMID,
            observed.data(), &observed_size) == SRT_ERROR) {
        std::cerr << "SRTO_STREAMID query failed: "
                  << srt_getlasterror_str() << '\n';
        return false;
    }
    if (observed_size < 0
        || observed_size > static_cast<int>(observed.size())
        || std::string_view{observed.data(),
               static_cast<std::size_t>(observed_size)}
            != configuration.expected_stream_id) {
        std::cerr << "SRTO_STREAMID mismatch: observed_bytes="
                  << observed_size << " expected_bytes="
                  << configuration.expected_stream_id.size() << '\n';
        return false;
    }
    std::cout << "{\"event\":\"stream_id_match\",\"bytes\":"
              << observed_size << "}\n" << std::flush;
    return true;
}

bool verify_expected_maximum_payload_size(
    SRTSOCKET socket, const Configuration& configuration)
{
    if (configuration.expected_maximum_payload_size < 0) {
        return true;
    }
    std::int32_t observed = -1;
    int observed_size = static_cast<int>(sizeof(observed));
    if (srt_getsockflag(socket, SRTO_PAYLOADSIZE, &observed, &observed_size)
        == SRT_ERROR) {
        std::cerr << "SRTO_PAYLOADSIZE query failed: " << srt_getlasterror_str()
                  << '\n';
        return false;
    }
    if (observed_size != static_cast<int>(sizeof(observed))
        || observed != configuration.expected_maximum_payload_size) {
        std::cerr << "SRTO_PAYLOADSIZE mismatch: observed=" << observed
                  << " expected=" << configuration.expected_maximum_payload_size
                  << '\n';
        return false;
    }
    std::cout << "{\"event\":\"payload_size_match\",\"bytes\":" << observed
              << "}\n"
              << std::flush;
    return true;
}

#ifdef ENABLE_AEAD_API_PREVIEW
bool verify_expected_crypto_mode(
    SRTSOCKET socket, const Configuration& configuration)
{
    if (configuration.expected_crypto_mode < 0) {
        return true;
    }
    std::int32_t observed = -1;
    int observed_size = static_cast<int>(sizeof(observed));
    if (srt_getsockflag(socket, SRTO_CRYPTOMODE, &observed, &observed_size)
        == SRT_ERROR) {
        std::cerr << "SRTO_CRYPTOMODE query failed: " << srt_getlasterror_str()
                  << '\n';
        return false;
    }
    if (observed_size != static_cast<int>(sizeof(observed))
        || observed != configuration.expected_crypto_mode) {
        std::cerr << "SRTO_CRYPTOMODE mismatch: observed=" << observed
                  << " expected=" << configuration.expected_crypto_mode << '\n';
        return false;
    }
    std::cout << "{\"event\":\"crypto_mode_match\",\"mode\":" << observed
              << "}\n"
              << std::flush;
    return true;
}
#endif

struct ListenCallbackContext {
    std::string_view expected_stream_id;
    std::string_view passphrase;
    int rejection_reason = -1;
    int connection_timeout_milliseconds = 0;
    bool called = false;
    bool valid = false;
};

struct ConnectCallbackContext {
    SRTSOCKET expected_socket = SRT_INVALID_SOCK;
    int expected_error = SRT_SUCCESS;
    std::atomic_bool called = false;
    std::atomic_bool valid = false;
    std::atomic_int error_code = SRT_SUCCESS;
    std::atomic_int peer_family = AF_UNSPEC;
    std::atomic_int token = 0;
    std::atomic_int socket_state = SRTS_NONEXIST;
    std::atomic_bool peer_name_available = false;
};

void validate_connect_callback(
    void* opaque,
    SRTSOCKET socket,
    int error_code,
    const sockaddr* peer_address,
    int token)
{
    auto& context = *static_cast<ConnectCallbackContext*>(opaque);
    const SRT_SOCKSTATUS state = srt_getsockstate(socket);
    sockaddr_storage connected_peer{};
    int connected_peer_size = static_cast<int>(sizeof(connected_peer));
    const bool peer_name_available =
        srt_getpeername(socket,
            reinterpret_cast<sockaddr*>(&connected_peer),
            &connected_peer_size)
        != SRT_ERROR;
    const int peer_family = peer_address != nullptr
        ? peer_address->sa_family
        : AF_UNSPEC;
    const bool success = error_code == SRT_SUCCESS;
    const bool valid = socket == context.expected_socket
        && error_code == context.expected_error
        && (peer_family == AF_INET || peer_family == AF_INET6)
        && token == -1
        && (success
                ? state == SRTS_CONNECTED && peer_name_available
                : state != SRTS_NONEXIST);
    context.error_code.store(error_code, std::memory_order_relaxed);
    context.peer_family.store(peer_family, std::memory_order_relaxed);
    context.token.store(token, std::memory_order_relaxed);
    context.socket_state.store(
        static_cast<int>(state), std::memory_order_relaxed);
    context.peer_name_available.store(
        peer_name_available, std::memory_order_relaxed);
    context.valid.store(valid, std::memory_order_release);
    context.called.store(true, std::memory_order_release);
}

void print_connect_callback_observation(
    const ConnectCallbackContext& context)
{
    if (!context.called.load(std::memory_order_acquire)) {
        std::cout
            << "{\"event\":\"connect_callback_observation\","
               "\"completed\":false}\n"
            << std::flush;
        return;
    }
    const bool valid = context.valid.load(std::memory_order_acquire);
    std::cout << "{\"event\":\"connect_callback\",\"valid\":"
              << (valid ? "true" : "false")
              << ",\"error_code\":"
              << context.error_code.load(std::memory_order_relaxed)
              << ",\"peer_family\":"
              << context.peer_family.load(std::memory_order_relaxed)
              << ",\"token\":"
              << context.token.load(std::memory_order_relaxed)
              << ",\"socket_state\":"
              << context.socket_state.load(std::memory_order_relaxed)
              << ",\"peer_name_available\":"
              << (context.peer_name_available.load(
                      std::memory_order_relaxed)
                      ? "true" : "false")
              << "}\n" << std::flush;
}

bool wait_for_connected_socket(
    SRTSOCKET socket,
    const ConnectCallbackContext& callback_context,
    Clock::time_point deadline)
{
    while (Clock::now() < deadline) {
        const SRT_SOCKSTATUS state = srt_getsockstate(socket);
        if (state == SRTS_CONNECTED) {
            return true;
        }
        if (state == SRTS_BROKEN || state == SRTS_CLOSED
            || state == SRTS_NONEXIST
            || (callback_context.called.load(std::memory_order_acquire)
                && !callback_context.valid.load(
                    std::memory_order_acquire))) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return srt_getsockstate(socket) == SRTS_CONNECTED;
}

bool wait_for_connect_callback(
    const ConnectCallbackContext& context,
    Clock::time_point deadline)
{
    while (!context.called.load(std::memory_order_acquire)
        && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return context.called.load(std::memory_order_acquire)
        && context.valid.load(std::memory_order_acquire);
}

int validate_listen_callback(
    void* opaque,
    SRTSOCKET socket,
    int handshake_version,
    const sockaddr* peer_address,
    const char* stream_id)
{
    auto& context = *static_cast<ListenCallbackContext*>(opaque);
    context.called = true;
    const std::string_view observed_stream_id =
        stream_id != nullptr ? std::string_view{stream_id}
                             : std::string_view{};
    const int peer_family =
        peer_address != nullptr ? peer_address->sa_family : AF_UNSPEC;
    const bool peer_is_ip =
        peer_family == AF_INET || peer_family == AF_INET6;
    const bool option_updated =
        srt_setsockflag(socket, SRTO_CONNTIMEO,
            &context.connection_timeout_milliseconds,
            static_cast<int>(
                sizeof(context.connection_timeout_milliseconds)))
        != SRT_ERROR;
    const bool security_updated = context.passphrase.empty()
        || srt_setsockflag(socket, SRTO_PASSPHRASE,
               context.passphrase.data(),
               static_cast<int>(context.passphrase.size()))
            != SRT_ERROR;
    bool rejection_reason_updated = true;
    if (context.rejection_reason >= 0) {
        rejection_reason_updated =
            srt_setrejectreason(socket, context.rejection_reason) != SRT_ERROR;
    }
    context.valid = handshake_version == 5
        && peer_is_ip
        && observed_stream_id == context.expected_stream_id
        && option_updated
        && security_updated
        && rejection_reason_updated;
    std::cout << "{\"event\":\"listen_callback\",\"valid\":"
              << (context.valid ? "true" : "false")
              << ",\"handshake_version\":" << handshake_version
              << ",\"peer_family\":" << peer_family
              << ",\"stream_id_bytes\":"
              << observed_stream_id.size()
              << ",\"option_updated\":"
              << (option_updated ? "true" : "false")
              << ",\"security_updated\":"
              << (security_updated ? "true" : "false")
              << ",\"rejected\":"
              << (context.rejection_reason >= 0 ? "true" : "false")
              << ",\"reject_reason\":"
              << (context.rejection_reason >= 0
                      ? context.rejection_reason
                      : 0)
              << "}\n" << std::flush;
    if (!context.valid || context.rejection_reason >= 0) {
        return SRT_ERROR;
    }
    return 0;
}

SRTSOCKET create_socket_with_minimum_isn(
    const Configuration& configuration)
{
    if (configuration.minimum_initial_sequence < 0) {
        return srt_create_socket();
    }

    constexpr int cleanup_interval = 1'024;
    for (int attempt = 1;
         attempt <= configuration.initial_sequence_search_limit;
         ++attempt) {
        const SRTSOCKET candidate = srt_create_socket();
        if (candidate == SRT_INVALID_SOCK) {
            std::cerr << "socket creation failed during ISN search\n";
            return SRT_INVALID_SOCK;
        }
        std::int32_t initial_sequence = -1;
        int initial_sequence_size =
            static_cast<int>(sizeof(initial_sequence));
        if (srt_getsockflag(
                candidate, SRTO_ISN, &initial_sequence,
                &initial_sequence_size)
            == SRT_ERROR) {
            std::cerr << "SRTO_ISN query failed during search: "
                      << srt_getlasterror_str() << '\n';
            (void)srt_close(candidate);
            return SRT_INVALID_SOCK;
        }
        if (initial_sequence
            >= configuration.minimum_initial_sequence) {
            std::cout
                << "{\"event\":\"socket_selected\",\"isn\":"
                << initial_sequence << ",\"attempts\":" << attempt
                << "}\n"
                << std::flush;
            return candidate;
        }
        (void)srt_close(candidate);

        if (attempt % cleanup_interval == 0) {
            if (srt_cleanup() == SRT_ERROR
                || srt_startup() == SRT_ERROR) {
                std::cerr
                    << "SRT lifecycle reset failed during ISN search\n";
                return SRT_INVALID_SOCK;
            }
        }
    }
    std::cerr << "no socket reached minimum SRTO_ISN "
              << configuration.minimum_initial_sequence
              << " within "
              << configuration.initial_sequence_search_limit
              << " attempts\n";
    return SRT_INVALID_SOCK;
}

struct EndpointAddress {
    sockaddr_storage storage{};
    int size = 0;

    [[nodiscard]] sockaddr* data() noexcept
    {
        return reinterpret_cast<sockaddr*>(&storage);
    }

    [[nodiscard]] const sockaddr* data() const noexcept
    {
        return reinterpret_cast<const sockaddr*>(&storage);
    }
};

bool make_endpoint(
    const std::string& host, std::uint16_t port,
    EndpointAddress& endpoint)
{
    endpoint = {};
    sockaddr_in ipv4{};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &ipv4.sin_addr) == 1) {
        std::memcpy(&endpoint.storage, &ipv4, sizeof(ipv4));
        endpoint.size = static_cast<int>(sizeof(ipv4));
        return true;
    }

    sockaddr_in6 ipv6{};
    ipv6.sin6_family = AF_INET6;
    ipv6.sin6_port = htons(port);
    if (inet_pton(AF_INET6, host.c_str(), &ipv6.sin6_addr) == 1) {
        std::memcpy(&endpoint.storage, &ipv6, sizeof(ipv6));
        endpoint.size = static_cast<int>(sizeof(ipv6));
        return true;
    }
    return false;
}

StatisticsSnapshot capture_statistics(SRTSOCKET socket)
{
    StatisticsSnapshot snapshot;
    snapshot.available =
        srt_bstats(socket, &snapshot.values, 0) != SRT_ERROR;
    return snapshot;
}

ReadinessOptionSnapshot capture_readiness_options(SRTSOCKET socket)
{
    ReadinessOptionSnapshot snapshot;
    const auto query = [socket](SRT_SOCKOPT option, std::int32_t& value) {
        int size = static_cast<int>(sizeof(value));
        return srt_getsockflag(socket, option, &value, &size) != SRT_ERROR
            && size == static_cast<int>(sizeof(value));
    };
    snapshot.available = query(SRTO_EVENT, snapshot.events)
        && query(SRTO_SNDDATA, snapshot.unacknowledged_send_packets)
        && query(SRTO_RCVDATA, snapshot.available_receive_packets);
    return snapshot;
}

NetworkOptionSnapshot capture_network_options(SRTSOCKET socket)
{
    NetworkOptionSnapshot snapshot;
    const auto query = [socket](SRT_SOCKOPT option, std::int32_t& value) {
        int size = static_cast<int>(sizeof(value));
        return srt_getsockflag(socket, option, &value, &size) != SRT_ERROR
            && size == static_cast<int>(sizeof(value));
    };
    snapshot.available = query(SRTO_IPTTL, snapshot.ip_time_to_live)
        && query(SRTO_IPTOS, snapshot.ip_type_of_service);
    return snapshot;
}

VersionTimingOptionSnapshot capture_version_timing_options(
    SRTSOCKET socket)
{
    VersionTimingOptionSnapshot snapshot;
    const auto query = [socket](SRT_SOCKOPT option, auto& value) {
        int size = static_cast<int>(sizeof(value));
        return srt_getsockflag(socket, option, &value, &size) != SRT_ERROR
            && size == static_cast<int>(sizeof(value));
    };
    snapshot.connection_time = srt_connection_time(socket);
    snapshot.current_time = srt_time_now();
    snapshot.available = query(SRTO_DRIFTTRACER, snapshot.drift_tracer)
        && query(SRTO_MININPUTBW, snapshot.minimum_input_bandwidth)
        && query(SRTO_MINVERSION, snapshot.minimum_peer_version)
        && snapshot.connection_time >= 0
        && snapshot.connection_time <= snapshot.current_time;
    return snapshot;
}

void print_event(
    const char* event, const char* role, std::uint64_t byte_count,
    Clock::duration elapsed, SRTSOCKET socket,
    const StatisticsSnapshot* captured_statistics = nullptr)
{
    StatisticsSnapshot current_statistics;
    if (captured_statistics == nullptr) {
        current_statistics = capture_statistics(socket);
        captured_statistics = &current_statistics;
    }
    const auto& statistics = *captured_statistics;
    const ReadinessOptionSnapshot readiness =
        capture_readiness_options(socket);
    const NetworkOptionSnapshot network =
        capture_network_options(socket);
    const VersionTimingOptionSnapshot version_timing =
        capture_version_timing_options(socket);
    std::int32_t peer_version = 0;
    int peer_version_size = static_cast<int>(sizeof(peer_version));
    const bool peer_version_available =
        srt_getsockflag(socket, SRTO_PEERVERSION,
            &peer_version, &peer_version_size) != SRT_ERROR
        && peer_version_size == static_cast<int>(sizeof(peer_version));
    std::int32_t initial_sequence = 0;
    int initial_sequence_size =
        static_cast<int>(sizeof(initial_sequence));
    const bool initial_sequence_available =
        srt_getsockflag(
            socket, SRTO_ISN, &initial_sequence,
            &initial_sequence_size)
        != SRT_ERROR;
    const auto elapsed_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    std::cout << "{\"event\":\"" << event << "\",\"role\":\"" << role
              << "\",\"bytes\":" << byte_count
              << ",\"elapsed_us\":" << elapsed_microseconds
              << ",\"srt_version\":" << srt_getversion();
    if (initial_sequence_available) {
        std::cout << ",\"isn\":" << initial_sequence;
    }
    if (peer_version_available) {
        std::cout << ",\"peer_version\":" << peer_version;
    }
    if (statistics.available) {
        std::cout << ",\"stats\":{\"pktSentTotal\":"
                  << statistics.values.pktSentTotal
                  << ",\"pktRecvTotal\":" << statistics.values.pktRecvTotal
                  << ",\"pktSndLossTotal\":"
                  << statistics.values.pktSndLossTotal
                  << ",\"pktRcvLossTotal\":"
                  << statistics.values.pktRcvLossTotal
                  << ",\"pktRetransTotal\":"
                  << statistics.values.pktRetransTotal
                  << ",\"pktSndDropTotal\":"
                  << statistics.values.pktSndDropTotal
                  << ",\"pktRcvDropTotal\":"
                  << statistics.values.pktRcvDropTotal
                  << ",\"pktSentACKTotal\":"
                  << statistics.values.pktSentACKTotal
                  << ",\"pktRecvACKTotal\":"
                  << statistics.values.pktRecvACKTotal
                  << ",\"pktSentNAKTotal\":"
                  << statistics.values.pktSentNAKTotal
                  << ",\"pktRecvNAKTotal\":"
                  << statistics.values.pktRecvNAKTotal
                  << ",\"pktRcvUndecryptTotal\":"
                  << statistics.values.pktRcvUndecryptTotal
                  << ",\"pktSndBuf\":"
                  << statistics.values.pktSndBuf
                  << ",\"pktRcvBuf\":"
                  << statistics.values.pktRcvBuf
                  << ",\"pktReorderDistance\":"
                  << statistics.values.pktReorderDistance
                  << ",\"pktReorderTolerance\":"
                  << statistics.values.pktReorderTolerance
                  << ",\"pktRcvBelated\":"
                  << statistics.values.pktRcvBelated
                  << ",\"pktRcvAvgBelatedTime\":"
                  << statistics.values.pktRcvAvgBelatedTime
                  << ",\"pktSndFilterExtraTotal\":"
                  << statistics.values.pktSndFilterExtraTotal
                  << ",\"pktRcvFilterExtraTotal\":"
                  << statistics.values.pktRcvFilterExtraTotal
                  << ",\"pktRcvFilterSupplyTotal\":"
                  << statistics.values.pktRcvFilterSupplyTotal
                  << ",\"pktRcvFilterLossTotal\":"
                  << statistics.values.pktRcvFilterLossTotal
                  << ",\"byteSentTotal\":"
                  << statistics.values.byteSentTotal
                  << ",\"byteRecvTotal\":"
                  << statistics.values.byteRecvTotal
                  << ",\"byteRcvLossTotal\":"
                  << statistics.values.byteRcvLossTotal
                  << ",\"byteRetransTotal\":"
                  << statistics.values.byteRetransTotal
                  << ",\"byteSndDropTotal\":"
                  << statistics.values.byteSndDropTotal
                  << ",\"byteRcvDropTotal\":"
                  << statistics.values.byteRcvDropTotal
                  << ",\"pktSentUniqueTotal\":"
                  << statistics.values.pktSentUniqueTotal
                  << ",\"pktRecvUniqueTotal\":"
                  << statistics.values.pktRecvUniqueTotal
                  << ",\"byteSentUniqueTotal\":"
                  << statistics.values.byteSentUniqueTotal
                  << ",\"byteRecvUniqueTotal\":"
                  << statistics.values.byteRecvUniqueTotal
                  << ",\"byteMSS\":"
                  << statistics.values.byteMSS << '}';
    }
    if (readiness.available) {
        std::cout << ",\"readiness\":{\"event\":"
                  << readiness.events
                  << ",\"snddata\":"
                  << readiness.unacknowledged_send_packets
                  << ",\"rcvdata\":"
                  << readiness.available_receive_packets << '}';
    }
    if (network.available) {
        std::cout << ",\"network\":{\"ipttl\":"
                  << network.ip_time_to_live
                  << ",\"iptos\":"
                  << network.ip_type_of_service << '}';
    }
    if (version_timing.available) {
        std::cout << ",\"version_timing\":{\"drift_tracer\":"
                  << (version_timing.drift_tracer ? "true" : "false")
                  << ",\"minimum_input_bandwidth\":"
                  << version_timing.minimum_input_bandwidth
                  << ",\"minimum_peer_version\":"
                  << version_timing.minimum_peer_version
                  << ",\"connection_time\":"
                  << version_timing.connection_time
                  << ",\"current_time\":"
                  << version_timing.current_time << '}';
    }
    std::cout << "}\n" << std::flush;
}

void print_complete(
    const char* role, std::uint64_t byte_count, Clock::duration elapsed,
    SRTSOCKET socket,
    const StatisticsSnapshot* captured_statistics = nullptr)
{
    print_event("complete", role, byte_count, elapsed, socket,
        captured_statistics);
}

void print_failure(
    const char* role, std::uint64_t byte_count, Clock::duration elapsed,
    SRTSOCKET socket)
{
    print_event("failure", role, byte_count, elapsed, socket);
}

std::int64_t monotonic_microseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch())
        .count();
}

struct SourceTimingEvent {
    std::uint64_t message_index = 0;
    std::int64_t monotonic_microseconds = 0;
    std::int64_t source_time_microseconds = 0;
    int payload_bytes = 0;
};

struct ReleaseTimingEvent {
    std::uint64_t message_index = 0;
    std::int64_t monotonic_microseconds = 0;
    std::int64_t srt_time_microseconds = 0;
    std::int64_t deadline_microseconds = 0;
    int payload_bytes = 0;
};

void print_source_timing_events(const std::vector<SourceTimingEvent>& events)
{
    for (const auto& event : events) {
        std::cout << "{\"event\":\"timing_source\",\"message_index\":"
                  << event.message_index << ",\"clock_domain\":\"steady_clock\""
                  << ",\"source_submission_monotonic_us\":"
                  << event.monotonic_microseconds
                  << ",\"source_protocol_time_us\":"
                  << event.source_time_microseconds
                  << ",\"payload_bytes\":" << event.payload_bytes << "}\n";
    }
    std::cout << std::flush;
}

void print_release_timing_events(const std::vector<ReleaseTimingEvent>& events)
{
    for (const auto& event : events) {
        std::cout << "{\"event\":\"timing_release\",\"message_index\":"
                  << event.message_index << ",\"clock_domain\":\"steady_clock\""
                  << ",\"release_monotonic_us\":"
                  << event.monotonic_microseconds
                  << ",\"release_srt_time_us\":" << event.srt_time_microseconds
                  << ",\"tsbpd_deadline_microseconds\":"
                  << event.deadline_microseconds
                  << ",\"payload_bytes\":" << event.payload_bytes << "}\n";
    }
    std::cout << std::flush;
}

constexpr int receive_contract_timeout_milliseconds = 150;
constexpr int receive_contract_short_buffer_bytes = 188;

int receive_once(SRTSOCKET socket, bool message_api, char* buffer, int capacity)
{
    return message_api ? srt_recvmsg(socket, buffer, capacity)
                       : srt_recv(socket, buffer, capacity);
}

bool probe_empty_receive_contract(
    SRTSOCKET socket, const Configuration& configuration, const char* role)
{
    std::array<char, 1'500> buffer {};
    const bool message_api = uses_message_api(configuration);
    const bool asynchronous = false;
    const bool synchronous = true;
    const auto nonblocking_started = Clock::now();
    const bool nonblocking_option =
        set_option(socket, SRTO_RCVSYN, asynchronous);
    const int nonblocking_result = nonblocking_option
        ? receive_once(socket, message_api, buffer.data(), buffer.size())
        : SRT_ERROR;
    const int nonblocking_error = nonblocking_result == SRT_ERROR
        ? srt_getlasterror(nullptr)
        : SRT_SUCCESS;
    const auto nonblocking_elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - nonblocking_started);

    const bool blocking_options = set_option(socket, SRTO_RCVSYN, synchronous)
        && set_option(
            socket, SRTO_RCVTIMEO, receive_contract_timeout_milliseconds);
    const auto timeout_started = Clock::now();
    const int timeout_result = blocking_options
        ? receive_once(socket, message_api, buffer.data(), buffer.size())
        : SRT_ERROR;
    const int timeout_error =
        timeout_result == SRT_ERROR ? srt_getlasterror(nullptr) : SRT_SUCCESS;
    const auto timeout_elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - timeout_started);
    const bool restored =
        set_option(socket, SRTO_RCVTIMEO, configuration.timeout_milliseconds);
    const bool matched = nonblocking_option && blocking_options && restored
        && nonblocking_result == SRT_ERROR && nonblocking_error == SRT_EASYNCRCV
        && nonblocking_elapsed < std::chrono::milliseconds {500}
        && timeout_result == SRT_ERROR && timeout_error == SRT_ETIMEOUT
        && timeout_elapsed >= std::chrono::milliseconds {50}
        && timeout_elapsed < std::chrono::seconds {5};
    std::cout << "{\"event\":\"receive_contract\",\"role\":\"" << role
              << "\",\"matched\":" << (matched ? "true" : "false")
              << ",\"message_api\":" << (message_api ? "true" : "false")
              << ",\"nonblocking_result\":" << nonblocking_result
              << ",\"nonblocking_error\":" << nonblocking_error
              << ",\"nonblocking_elapsed_us\":" << nonblocking_elapsed.count()
              << ",\"timeout_result\":" << timeout_result
              << ",\"timeout_error\":" << timeout_error
              << ",\"timeout_elapsed_us\":" << timeout_elapsed.count()
              << ",\"timeout_configured_ms\":"
              << receive_contract_timeout_milliseconds << "}\n"
              << std::flush;
    return matched;
}

bool wait_for_contract_barrier(
    const char* event, const char* role, const char* release)
{
    std::cout << "{\"event\":\"" << event << "\",\"role\":\"" << role << "\"}\n"
              << std::flush;
    std::string command;
    return std::getline(std::cin, command) && command == release;
}

bool wait_for_broken_socket(SRTSOCKET socket,
    const Configuration& configuration, SRT_SOCKSTATUS& observed_state)
{
    const auto deadline = Clock::now()
        + std::chrono::milliseconds {configuration.timeout_milliseconds};
    do {
        observed_state = srt_getsockstate(socket);
        if (observed_state == SRTS_BROKEN) {
            return true;
        }
        if (observed_state == SRTS_CLOSED || observed_state == SRTS_NONEXIST) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    } while (Clock::now() < deadline);
    observed_state = srt_getsockstate(socket);
    return observed_state == SRTS_BROKEN;
}

bool prepare_receive_contract_drain(
    SRTSOCKET socket, const Configuration& configuration, const char* role)
{
    if (!probe_empty_receive_contract(socket, configuration, role)
        || !wait_for_contract_barrier(
            "receive_contract_receiver_ready", role, "receive")) {
        return false;
    }
    SRT_SOCKSTATUS state = SRTS_NONEXIST;
    const bool broken = wait_for_broken_socket(socket, configuration, state);
    int short_result = 0;
    int short_error = SRT_SUCCESS;
    bool short_rejected = true;
    if (uses_message_api(configuration)) {
        std::array<char, receive_contract_short_buffer_bytes> buffer {};
        short_result =
            srt_recvmsg(socket, buffer.data(), static_cast<int>(buffer.size()));
        short_error =
            short_result == SRT_ERROR ? srt_getlasterror(nullptr) : SRT_SUCCESS;
        short_rejected =
            short_result == SRT_ERROR && short_error == SRT_EINVALMSGAPI;
    }
    const bool matched = broken && short_rejected;
    std::cout << "{\"event\":\"terminal_drain_ready\",\"role\":\"" << role
              << "\",\"matched\":" << (matched ? "true" : "false")
              << ",\"message_api\":"
              << (uses_message_api(configuration) ? "true" : "false")
              << ",\"socket_state\":" << static_cast<int>(state)
              << ",\"short_result\":" << short_result
              << ",\"short_error\":" << short_error
              << ",\"short_buffer_bytes\":"
              << receive_contract_short_buffer_bytes << "}\n"
              << std::flush;
    return matched;
}

int receive_payload(
    SRTSOCKET socket, const Configuration& configuration,
    const char* role)
{
    std::ofstream output{
        configuration.output_path, std::ios::binary | std::ios::trunc};
    if (!output) {
        std::cerr << "cannot open output file\n";
        return 5;
    }
    std::array<char, 65'536> buffer{};
    std::uint64_t received_total = 0;
    std::uint64_t message_index = 0;
    std::vector<ReleaseTimingEvent> timing_events;
    const auto started = Clock::now();
    while (received_total < configuration.byte_count) {
        const auto remaining = configuration.byte_count - received_total;
        const int capacity = configuration.receive_size;
        SRT_MSGCTRL control = srt_msgctrl_default;
        const int received = configuration.measure_timing
            ? srt_recvmsg2(socket, buffer.data(), capacity, &control)
            : uses_message_api(configuration)
            ? srt_recvmsg(socket, buffer.data(), capacity)
            : srt_recv(socket, buffer.data(), capacity);
        const std::int64_t release_monotonic =
            configuration.measure_timing ? monotonic_microseconds() : 0;
        const std::int64_t release_srt_time =
            configuration.measure_timing ? srt_time_now() : 0;
        if (received == SRT_ERROR) {
            std::cerr << "receive failed after " << received_total
                      << " bytes: " << srt_getlasterror_str() << '\n';
            print_failure(
                role, received_total, Clock::now() - started, socket);
            return 6;
        }
        if (received <= 0) {
            std::cerr << "unexpected end of stream\n";
            print_failure(
                role, received_total, Clock::now() - started, socket);
            return 6;
        }
        if (static_cast<std::uint64_t>(received) > remaining) {
            std::cerr << "received message exceeds expected byte count\n";
            print_failure(
                role, received_total, Clock::now() - started, socket);
            return 6;
        }
        output.write(buffer.data(), received);
        if (!output) {
            std::cerr << "output write failed\n";
            print_failure(
                role, received_total, Clock::now() - started, socket);
            return 5;
        }
        received_total += static_cast<std::uint64_t>(received);
        if (configuration.measure_timing) {
            timing_events.push_back(ReleaseTimingEvent {
                message_index,
                release_monotonic,
                release_srt_time,
                control.srctime,
                received,
            });
            ++message_index;
        }
    }
    output.flush();
    if (!output) {
        std::cerr << "output flush failed\n";
        print_failure(
            role, received_total, Clock::now() - started, socket);
        return 5;
    }
    // Preserve a complete receive snapshot while the connection is known to
    // be live. The peer may close during the grace period, making a later
    // srt_bstats query fail even though every payload byte was delivered.
    StatisticsSnapshot completion_statistics = capture_statistics(socket);
    if (configuration.expect_peer_shutdown) {
        const int terminal_result = uses_message_api(configuration)
            ? srt_recvmsg(
                  socket, buffer.data(), static_cast<int>(buffer.size()))
            : srt_recv(socket, buffer.data(), 1);
        const int terminal_error = terminal_result == SRT_ERROR
            ? srt_getlasterror(nullptr)
            : SRT_SUCCESS;
        const SRT_SOCKSTATUS terminal_state = srt_getsockstate(socket);
        const bool matched = uses_message_api(configuration)
            ? terminal_result == SRT_ERROR && terminal_error == SRT_ECONNLOST
                && terminal_state == SRTS_BROKEN
            : terminal_result == 0 && terminal_error == SRT_SUCCESS
                && terminal_state == SRTS_BROKEN;
        std::cout << "{\"event\":\"peer_shutdown\",\"role\":\"" << role
                  << "\",\"matched\":" << (matched ? "true" : "false")
                  << ",\"bytes\":" << received_total << ",\"message_api\":"
                  << (uses_message_api(configuration) ? "true" : "false")
                  << ",\"terminal\":\""
                  << (uses_message_api(configuration) ? "connection-lost"
                                                      : "zero-byte-eof")
                  << "\""
                  << ",\"result\":" << terminal_result
                  << ",\"error\":" << terminal_error
                  << ",\"socket_state\":" << static_cast<int>(terminal_state)
                  << "}\n"
                  << std::flush;
        if (!matched) {
            std::cerr << "expected peer SHUTDOWN was not observed: result="
                      << terminal_result << " error=" << terminal_error
                      << " state=" << static_cast<int>(terminal_state) << '\n';
            print_failure(role, received_total, Clock::now() - started, socket);
            return 6;
        }
    }
    if (configuration.expect_peer_idle_timeout) {
        const auto terminal_started = Clock::now();
        const int terminal_result = uses_message_api(configuration)
            ? srt_recvmsg(
                  socket, buffer.data(), static_cast<int>(buffer.size()))
            : srt_recv(socket, buffer.data(), 1);
        const auto terminal_elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - terminal_started);
        const int terminal_error = terminal_result == SRT_ERROR
            ? srt_getlasterror(nullptr)
            : SRT_SUCCESS;
        const SRT_SOCKSTATUS terminal_state = srt_getsockstate(socket);
        const bool matched = terminal_result == SRT_ERROR
            && terminal_error == SRT_ECONNLOST && terminal_state == SRTS_BROKEN;
        std::cout << "{\"event\":\"peer_idle_timeout\",\"role\":\"" << role
                  << "\",\"matched\":" << (matched ? "true" : "false")
                  << ",\"bytes\":" << received_total << ",\"message_api\":"
                  << (uses_message_api(configuration) ? "true" : "false")
                  << ",\"terminal\":\"connection-lost\""
                  << ",\"result\":" << terminal_result
                  << ",\"error\":" << terminal_error
                  << ",\"socket_state\":" << static_cast<int>(terminal_state)
                  << ",\"peer_idle_timeout_ms\":"
                  << configuration.peer_idle_timeout_milliseconds
                  << ",\"terminal_wait_us\":" << terminal_elapsed.count()
                  << "}\n"
                  << std::flush;
        if (!matched) {
            std::cerr << "expected peer-idle timeout was not observed: result="
                      << terminal_result << " error=" << terminal_error
                      << " state=" << static_cast<int>(terminal_state) << '\n';
            print_failure(role, received_total, Clock::now() - started, socket);
            return 6;
        }
    }
    if (configuration.expect_eof) {
        const int received = srt_recv(socket, buffer.data(), 1);
        if (received != 0) {
            std::cerr << "expected zero-byte stream EOF, received="
                      << received;
            if (received == SRT_ERROR) {
                std::cerr << " error=" << srt_getlasterror_str();
            }
            std::cerr << '\n';
            print_failure(
                role, received_total, Clock::now() - started, socket);
            return 6;
        }
        std::cout << "{\"event\":\"eof\",\"role\":\""
                  << role << "\",\"bytes\":" << received_total
                  << "}\n" << std::flush;
    }
    if (configuration.shutdown_grace_milliseconds != 0) {
        // Application delivery can precede the peer's observation of the
        // final cumulative ACK. Keep the connected socket alive briefly so
        // the SRT runtime can complete its acknowledgement exchange.
        std::this_thread::sleep_for(std::chrono::milliseconds{
            configuration.shutdown_grace_milliseconds});
    }
    const auto final_statistics = capture_statistics(socket);
    if (final_statistics.available) {
        completion_statistics = final_statistics;
    }
    if (configuration.measure_timing) {
        print_release_timing_events(timing_events);
    }
    print_complete(role, received_total, Clock::now() - started, socket,
        &completion_statistics);
    return 0;
}

int send_payload(
    SRTSOCKET socket, const Configuration& configuration,
    const char* role)
{
    std::ifstream input{configuration.input_path, std::ios::binary};
    if (!input) {
        std::cerr << "cannot open input file\n";
        return 5;
    }
    std::array<char, 65'536> buffer{};
    std::uint64_t sent_total = 0;
    std::uint64_t message_index = 0;
    std::vector<SourceTimingEvent> timing_events;
    const auto started = Clock::now();
    const std::int64_t source_time_origin =
        configuration.explicit_source_time ? srt_time_now() : 0;
    const bool changes_maximum_bandwidth =
        configuration.second_maximum_bandwidth >= 0;
    const std::uint64_t first_phase_bytes =
        changes_maximum_bandwidth
        ? configuration.byte_count / 2U
        : 0U;
    Clock::duration first_phase_elapsed{};
    Clock::time_point second_phase_started{};
    bool maximum_bandwidth_changed = false;
    int injected_peer_error_count = 0;
    bool recovered_after_injected_peer_error = false;
    std::uint64_t injected_peer_error_offset = 0;
    StatisticsSnapshot completion_statistics;

    if (configuration.explicit_source_time) {
        std::cout
            << "{\"event\":\"source_timeline\",\"role\":\""
            << role << "\",\"origin_microseconds\":"
            << source_time_origin << ",\"bytes_per_second\":"
            << configuration.input_bandwidth << "}\n"
            << std::flush;
    }

    const auto source_time_at = [&](std::uint64_t byte_offset) {
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::duration<long double>{
            static_cast<long double>(byte_offset)
            / static_cast<long double>(configuration.input_bandwidth)});
        return source_time_origin + elapsed.count();
    };

    const auto pace_source = [&]() {
        if (!configuration.source_pacing) {
            return;
        }
        const auto source_elapsed =
            std::chrono::duration<long double>{
                static_cast<long double>(sent_total)
                / static_cast<long double>(
                    configuration.input_bandwidth)};
        std::this_thread::sleep_until(
            started
            + std::chrono::duration_cast<Clock::duration>(
                source_elapsed));
    };

    const auto drain_send_buffer = [&]() -> bool {
        const auto drain_deadline = Clock::now()
            + std::chrono::milliseconds{
                configuration.timeout_milliseconds};
        for (;;) {
            std::size_t blocks = 0;
            std::size_t bytes = 0;
            if (srt_getsndbuffer(socket, &blocks, &bytes) == SRT_ERROR) {
                std::cerr << "send-buffer query failed: "
                          << srt_getlasterror_str() << '\n';
                print_failure(
                    role, sent_total, Clock::now() - started, socket);
                return false;
            }
            const auto current_statistics = capture_statistics(socket);
            if (current_statistics.available) {
                completion_statistics = current_statistics;
            }
            if (blocks == 0U) {
                return true;
            }
            if (Clock::now() >= drain_deadline) {
                std::cerr << "send buffer did not drain; blocks="
                          << blocks << " bytes=" << bytes << '\n';
                print_failure(
                    role, sent_total, Clock::now() - started, socket);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    };

    while (sent_total < configuration.byte_count) {
        const auto remaining = configuration.byte_count - sent_total;
        const auto read_size = static_cast<std::streamsize>(
            std::min<std::uint64_t>(
                static_cast<std::uint64_t>(configuration.chunk_size),
                remaining));
        input.read(buffer.data(), read_size);
        const auto read = input.gcount();
        if (read <= 0) {
            std::cerr << "input ended after " << sent_total << " bytes\n";
            return 5;
        }
        int offset = 0;
        while (offset < read) {
            pace_source();
            const int requested = static_cast<int>(read) - offset;
            const std::int64_t source_monotonic =
                configuration.measure_timing ? monotonic_microseconds() : 0;
            const std::int64_t source_protocol_time =
                configuration.explicit_source_time ? source_time_at(sent_total)
                                                   : 0;
            int sent = SRT_ERROR;
            if (uses_message_api(configuration)) {
                if (configuration.explicit_source_time) {
                    SRT_MSGCTRL control = srt_msgctrl_default;
                    control.inorder = 1;
                    control.srctime = source_time_at(sent_total);
                    sent = srt_sendmsg2(
                        socket, buffer.data() + offset, requested, &control);
                } else {
                    sent = srt_sendmsg(
                        socket, buffer.data() + offset, requested, -1, 1);
                }
            } else {
                sent = srt_send(socket, buffer.data() + offset, requested);
            }
            if (sent == SRT_ERROR) {
                const int error = srt_getlasterror(nullptr);
                if (configuration.expect_injected_peer_error
                    && error == SRT_EPEERERR
                    && injected_peer_error_count == 0) {
                    if (srt_getsockstate(socket) != SRTS_CONNECTED) {
                        std::cerr
                            << "injected peer error broke the connection\n";
                        print_failure(role, sent_total,
                            Clock::now() - started, socket);
                        return 6;
                    }
                    injected_peer_error_count = 1;
                    injected_peer_error_offset = sent_total;
                    continue;
                }
                std::cerr << "send failed after " << sent_total
                          << " bytes: " << srt_getlasterror_str() << '\n';
                print_failure(
                    role, sent_total, Clock::now() - started, socket);
                return 6;
            }
            if (injected_peer_error_count == 1) {
                recovered_after_injected_peer_error = true;
            }
            if (sent <= 0
                || (uses_message_api(configuration) && sent != requested)) {
                std::cerr << "unexpected partial message send\n";
                print_failure(
                    role, sent_total, Clock::now() - started, socket);
                return 6;
            }
            offset += sent;
            sent_total += static_cast<std::uint64_t>(sent);
            if (configuration.measure_timing) {
                timing_events.push_back(SourceTimingEvent {
                    message_index,
                    source_monotonic,
                    source_protocol_time,
                    sent,
                });
                ++message_index;
            }
        }
        if (
            changes_maximum_bandwidth
            && !maximum_bandwidth_changed
            && sent_total >= first_phase_bytes
        ) {
            if (sent_total != first_phase_bytes) {
                std::cerr
                    << "dynamic bandwidth phase is not chunk-aligned\n";
                print_failure(
                    role, sent_total, Clock::now() - started, socket);
                return 7;
            }
            if (!drain_send_buffer()) {
                return 7;
            }
            const auto changed_at = Clock::now();
            first_phase_elapsed = changed_at - started;
            if (!set_option(
                    socket, SRTO_MAXBW,
                    configuration.second_maximum_bandwidth)) {
                print_failure(
                    role, sent_total, Clock::now() - started, socket);
                return 6;
            }
            maximum_bandwidth_changed = true;
            second_phase_started = changed_at;
            std::cout
                << "{\"event\":\"rate_change\",\"role\":\""
                << role << "\",\"bytes\":" << sent_total
                << ",\"old_max_bw\":"
                << configuration.maximum_bandwidth
                << ",\"new_max_bw\":"
                << configuration.second_maximum_bandwidth
                << "}\n"
                << std::flush;
        }
    }

    if (!drain_send_buffer()) {
        return 7;
    }
    if (configuration.expect_injected_peer_error) {
        const SRT_SOCKSTATUS state = srt_getsockstate(socket);
        const bool expected_missing =
            configuration.require_missing_injected_peer_error;
        const bool missing_allowed =
            configuration.allow_missing_injected_peer_error;
        const bool observed_missing = injected_peer_error_count == 0
            && !recovered_after_injected_peer_error;
        const bool observed_once = injected_peer_error_count == 1
            && recovered_after_injected_peer_error;
        bool observation_matches = observed_once;
        if (expected_missing) {
            observation_matches = observed_missing;
        } else if (missing_allowed) {
            observation_matches = observed_once || observed_missing;
        }
        const bool matched = state == SRTS_CONNECTED && observation_matches;
        std::cout << "{\"event\":\"peer_error\",\"role\":\"" << role
                  << "\",\"matched\":" << (matched ? "true" : "false")
                  << ",\"error\":" << SRT_EPEERERR
                  << ",\"count\":" << injected_peer_error_count
                  << ",\"offset\":" << injected_peer_error_offset
                  << ",\"recovered\":"
                  << (recovered_after_injected_peer_error ? "true" : "false")
                  << ",\"expected_missing\":"
                  << (expected_missing ? "true" : "false")
                  << ",\"missing_allowed\":"
                  << (missing_allowed ? "true" : "false")
                  << ",\"socket_state\":" << static_cast<int>(state) << "}\n"
                  << std::flush;
        if (!matched) {
            std::cerr << "injected peer-error observation was inconsistent: "
                      << "count=" << injected_peer_error_count
                      << " recovered=" << recovered_after_injected_peer_error
                      << " state=" << static_cast<int>(state) << '\n';
            return 7;
        }
    }
    const auto drained_at = Clock::now();
    if (configuration.measure_timing) {
        print_source_timing_events(timing_events);
    }
    if (changes_maximum_bandwidth) {
        if (!maximum_bandwidth_changed) {
            std::cerr << "dynamic bandwidth change was not applied\n";
            print_failure(
                role, sent_total, drained_at - started, socket);
            return 7;
        }
        const auto first_elapsed_microseconds =
            std::chrono::duration_cast<std::chrono::microseconds>(
                first_phase_elapsed).count();
        const auto second_elapsed_microseconds =
            std::chrono::duration_cast<std::chrono::microseconds>(
                drained_at - second_phase_started).count();
        std::cout
            << "{\"event\":\"rate_phases\",\"role\":\""
            << role << "\",\"first_bytes\":" << first_phase_bytes
            << ",\"first_elapsed_us\":"
            << first_elapsed_microseconds
            << ",\"first_max_bw\":"
            << configuration.maximum_bandwidth
            << ",\"second_bytes\":"
            << configuration.byte_count - first_phase_bytes
            << ",\"second_elapsed_us\":"
            << second_elapsed_microseconds
            << ",\"second_max_bw\":"
            << configuration.second_maximum_bandwidth
            << "}\n"
            << std::flush;
    }
    if (configuration.shutdown_grace_milliseconds != 0) {
        // A drained send buffer means the peer acknowledged the packets, not
        // that its TSBPD latency gate has released them to the application.
        // Keep the connection alive long enough to avoid pre-empting delivery
        // with SHUTDOWN when this black-box helper exits.
        std::this_thread::sleep_for(std::chrono::milliseconds{
            configuration.shutdown_grace_milliseconds});
    }
    // Pacing can make the final send counters visible after the send buffer
    // first reports empty. Prefer a fresh post-grace snapshot, while retaining
    // the last drain-loop snapshot if the final query is unavailable.
    const auto final_statistics = capture_statistics(socket);
    if (final_statistics.available) {
        completion_statistics = final_statistics;
    }
    if (configuration.transport == SRTT_LIVE) {
        if (!completion_statistics.available) {
            std::cerr << "sender completion statistics are unavailable\n";
            print_failure(
                role, sent_total, Clock::now() - started, socket);
            return 7;
        }
        const auto expected_packets =
            configuration.byte_count / configuration.chunk_size
            + (configuration.byte_count
                    % configuration.chunk_size
                != 0U
                ? 1U
                : 0U);
        const auto& statistics = completion_statistics.values;
        if (statistics.pktSndDropTotal != 0
            || statistics.pktSentTotal
                < static_cast<std::int64_t>(expected_packets)) {
            std::cerr << "sender completion validation failed: sent="
                      << statistics.pktSentTotal
                      << " expected=" << expected_packets
                      << " dropped=" << statistics.pktSndDropTotal
                      << '\n';
            print_event("failure", role, sent_total,
                Clock::now() - started, socket,
                &completion_statistics);
            return 7;
        }
    }
    print_complete(role, sent_total, Clock::now() - started, socket,
        &completion_statistics);
    return 0;
}

int receive_file_payload(
    SRTSOCKET socket, const Configuration& configuration,
    const char* role)
{
    std::int64_t offset = 0;
    const auto started = Clock::now();
    const std::int64_t received = srt_recvfile(
        socket, configuration.output_path.c_str(), &offset,
        static_cast<std::int64_t>(configuration.byte_count),
        configuration.chunk_size);
    const int error = received == SRT_ERROR
        ? srt_getlasterror(nullptr) : SRT_SUCCESS;
    const SRT_SOCKSTATUS state = srt_getsockstate(socket);

    if (configuration.expect_file_write_error) {
        const bool matched = received == SRT_ERROR
            && error == SRT_EWRPERM
            && state == SRTS_CONNECTED
            && offset >= 0
            && static_cast<std::uint64_t>(offset)
                < configuration.byte_count;
        std::cout
            << "{\"event\":\"file_write_error\",\"role\":\""
            << role << "\",\"matched\":"
            << (matched ? "true" : "false")
            << ",\"error\":" << error
            << ",\"offset\":" << offset
            << ",\"socket_state\":" << static_cast<int>(state)
            << "}\n" << std::flush;
        if (!matched) {
            std::cerr << "expected recvfile write failure was not observed: "
                      << "result=" << received << " error=" << error
                      << " offset=" << offset
                      << " state=" << static_cast<int>(state) << '\n';
            return 6;
        }
        if (configuration.shutdown_grace_milliseconds != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{
                configuration.shutdown_grace_milliseconds});
        }
        return 0;
    }

    if (received != static_cast<std::int64_t>(configuration.byte_count)
        || offset != static_cast<std::int64_t>(configuration.byte_count)) {
        std::cerr << "recvfile failed after " << offset
                  << " bytes: " << srt_getlasterror_str() << '\n';
        print_failure(role, static_cast<std::uint64_t>(offset),
            Clock::now() - started, socket);
        return 6;
    }
    if (configuration.shutdown_grace_milliseconds != 0) {
        // recvfile returning the full size proves application delivery, but
        // the final cumulative ACK can still be queued in the SRT runtime.
        // Keep the connected receiver alive so a public-API sender can drain
        // its last acknowledged blocks before this helper closes the socket.
        std::this_thread::sleep_for(std::chrono::milliseconds {
            configuration.shutdown_grace_milliseconds});
    }
    print_complete(role, static_cast<std::uint64_t>(received),
        Clock::now() - started, socket);
    return 0;
}

int send_file_payload(
    SRTSOCKET socket, const Configuration& configuration,
    const char* role)
{
    std::int64_t offset = 0;
    const auto started = Clock::now();
    const std::int64_t requested_size = configuration.file_size_to_eof
        ? -1
        : static_cast<std::int64_t>(configuration.byte_count);
    const std::int64_t sent =
        srt_sendfile(socket, configuration.input_path.c_str(), &offset,
            requested_size, configuration.chunk_size);
    const int error = sent == SRT_ERROR
        ? srt_getlasterror(nullptr) : SRT_SUCCESS;
    const SRT_SOCKSTATUS state_after_error = srt_getsockstate(socket);

    if (configuration.expect_peer_error) {
        const bool asynchronous = false;
        int followup_result = SRT_ERROR;
        int followup_error = SRT_SUCCESS;
        bool followup_attempted = false;
        if (set_option(socket, SRTO_SNDSYN, asynchronous)) {
            const char probe = 'p';
            followup_attempted = true;
            followup_result = srt_send(socket, &probe, 1);
            if (followup_result == SRT_ERROR) {
                followup_error = srt_getlasterror(nullptr);
            }
        } else {
            followup_error = srt_getlasterror(nullptr);
        }
        const SRT_SOCKSTATUS state_after_followup =
            srt_getsockstate(socket);
        const bool matched = sent == SRT_ERROR
            && error == SRT_EPEERERR
            && state_after_error == SRTS_CONNECTED
            && followup_attempted
            && followup_error != SRT_EPEERERR
            && state_after_followup == SRTS_CONNECTED
            && offset >= 0
            && static_cast<std::uint64_t>(offset)
                < configuration.byte_count;
        std::cout
            << "{\"event\":\"peer_error\",\"role\":\""
            << role << "\",\"matched\":"
            << (matched ? "true" : "false")
            << ",\"error\":" << error
            << ",\"offset\":" << offset
            << ",\"state_after_error\":"
            << static_cast<int>(state_after_error)
            << ",\"followup_attempted\":"
            << (followup_attempted ? "true" : "false")
            << ",\"followup_result\":" << followup_result
            << ",\"followup_error\":" << followup_error
            << ",\"state_after_followup\":"
            << static_cast<int>(state_after_followup)
            << "}\n" << std::flush;
        if (!matched) {
            std::cerr << "expected one-shot peer error was not observed: "
                      << "result=" << sent << " error=" << error
                      << " offset=" << offset
                      << " state=" << static_cast<int>(state_after_error)
                      << " followup=" << followup_result
                      << " followup_error=" << followup_error
                      << " followup_state="
                      << static_cast<int>(state_after_followup) << '\n';
            return 6;
        }
        return 0;
    }

    if (sent != static_cast<std::int64_t>(configuration.byte_count)
        || offset != static_cast<std::int64_t>(configuration.byte_count)) {
        std::cerr << "sendfile failed after " << offset
                  << " bytes: " << srt_getlasterror_str() << '\n';
        print_failure(role, static_cast<std::uint64_t>(offset),
            Clock::now() - started, socket);
        return 6;
    }
    StatisticsSnapshot completion_statistics;
    const auto retain_newest_sender_statistics =
        [&](const StatisticsSnapshot& candidate) {
            if (!candidate.available) {
                return;
            }
            if (!completion_statistics.available
                || candidate.values.byteSentUniqueTotal
                    > completion_statistics.values.byteSentUniqueTotal
                || (candidate.values.byteSentUniqueTotal
                        == completion_statistics.values.byteSentUniqueTotal
                    && candidate.values.pktSentUniqueTotal
                        > completion_statistics.values.pktSentUniqueTotal)) {
                // Cumulative sender counters cannot legitimately move
                // backwards. The pinned implementation can expose a smaller
                // late snapshot while its close path is settling, so retain
                // the newest monotonic high-water snapshot as one coherent
                // observation rather than mixing individual fields.
                completion_statistics = candidate;
            }
        };
    const auto drain_deadline = Clock::now()
        + std::chrono::milliseconds {configuration.timeout_milliseconds};
    for (;;) {
        std::size_t blocks = 0;
        std::size_t bytes = 0;
        if (srt_getsndbuffer(socket, &blocks, &bytes) == SRT_ERROR) {
            std::cerr << "sendfile buffer query failed: "
                      << srt_getlasterror_str() << '\n';
            print_failure(role, static_cast<std::uint64_t>(offset),
                Clock::now() - started, socket);
            return 7;
        }
        const auto current_statistics = capture_statistics(socket);
        retain_newest_sender_statistics(current_statistics);
        const bool statistics_drained = !current_statistics.available
            || current_statistics.values.pktSndBuf == 0;
        if (blocks == 0U && statistics_drained) {
            break;
        }
        if (Clock::now() >= drain_deadline) {
            std::cerr << "sendfile buffer did not drain; blocks=" << blocks
                      << " bytes=" << bytes << '\n';
            print_failure(role, static_cast<std::uint64_t>(offset),
                Clock::now() - started, socket);
            return 7;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    if (configuration.shutdown_grace_milliseconds != 0) {
        // Drain proves cumulative acknowledgement, while a short grace keeps
        // the peer alive long enough to publish its final API completion and
        // statistics before close. Retain the newest live sample instead of
        // relying on one query after that close.
        const auto deadline = Clock::now()
            + std::chrono::milliseconds {
                configuration.shutdown_grace_milliseconds};
        while (Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds {5});
            const auto current_statistics = capture_statistics(socket);
            retain_newest_sender_statistics(current_statistics);
        }
    }
    print_complete(role, static_cast<std::uint64_t>(sent),
        Clock::now() - started, socket, &completion_statistics);
    return 0;
}

int transfer_payload(
    SRTSOCKET socket, const Configuration& configuration, const char* role)
{
    if (configuration.receive_contract_sender
        && !wait_for_contract_barrier(
            "receive_contract_sender_ready", role, "send")) {
        std::cerr << "receive contract sender was not released\n";
        return 5;
    }
    if (configuration.receive_contract_receiver
        && !prepare_receive_contract_drain(socket, configuration, role)) {
        std::cerr << "receive contract drain preparation failed\n";
        return 6;
    }
    if (is_sender(configuration)) {
        if (configuration.sender_start_delay_milliseconds > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds {
                configuration.sender_start_delay_milliseconds});
        }
        return configuration.file_api
            ? send_file_payload(socket, configuration, role)
            : send_payload(socket, configuration, role);
    }
    return configuration.file_api
        ? receive_file_payload(socket, configuration, role)
        : receive_payload(socket, configuration, role);
}

int run_listener(const Configuration& configuration)
{
    SocketHandle listener{srt_create_socket()};
    EndpointAddress address;
    std::string callback_passphrase;
    if (!configuration.listen_callback_passphrase_environment.empty()) {
        const char* configured_passphrase = std::getenv(
            configuration.listen_callback_passphrase_environment.c_str());
        if (configured_passphrase == nullptr
            || *configured_passphrase == '\0') {
            std::cerr << "listener callback passphrase environment "
                         "variable is unset\n";
            return 3;
        }
        callback_passphrase = configured_passphrase;
    }
    ListenCallbackContext callback_context;
    callback_context.expected_stream_id =
        configuration.listen_callback_stream_id;
    callback_context.passphrase = callback_passphrase;
    callback_context.rejection_reason =
        configuration.listen_callback_rejection_reason;
    callback_context.connection_timeout_milliseconds =
        configuration.timeout_milliseconds;
    if (listener.value == SRT_INVALID_SOCK
        || !make_endpoint(
            configuration.host, configuration.port, address)
        || !configure_socket(listener.value, configuration)) {
        std::cerr << "listener setup failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }
    if (!configuration.listen_callback_stream_id.empty()
        && srt_listen_callback(listener.value,
               validate_listen_callback, &callback_context)
            == SRT_ERROR) {
        std::cerr << "listener callback registration failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }
    if (srt_bind(listener.value,
               address.data(), address.size)
            == SRT_ERROR
        || srt_listen(listener.value, 1) == SRT_ERROR) {
        std::cerr << "listener setup failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }
    std::cout << "{\"event\":\"ready\",\"port\":"
              << configuration.port << ",\"srt_version\":"
              << srt_getversion() << "}\n" << std::flush;
    sockaddr_storage peer_address{};
    int peer_size = static_cast<int>(sizeof(peer_address));
    SocketHandle connected{srt_accept(listener.value,
        reinterpret_cast<sockaddr*>(&peer_address), &peer_size)};
    if (connected.value == SRT_INVALID_SOCK) {
        std::cerr << "accept failed: " << srt_getlasterror_str() << '\n';
        return 4;
    }
    if (!verify_expected_stream_id(
            connected.value, configuration)) {
        return 7;
    }
    if (!verify_expected_maximum_payload_size(connected.value, configuration)) {
        return 7;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    if (!verify_expected_crypto_mode(connected.value, configuration)) {
        return 7;
    }
#endif
    if (!configuration.listen_callback_stream_id.empty()
        && (!callback_context.called || !callback_context.valid)) {
        std::cerr << "listener callback validation was not completed\n";
        return 7;
    }
    return transfer_payload(
        connected.value, configuration, role_name(configuration.role));
}

int run_caller(const Configuration& configuration)
{
    // The callback context must outlive SocketHandle: a failing validation
    // path closes and joins the background connect before the context dies.
    ConnectCallbackContext callback_context;
    SocketHandle socket{
        create_socket_with_minimum_isn(configuration)};
    EndpointAddress address;
    if (socket.value == SRT_INVALID_SOCK
        || !make_endpoint(
            configuration.host, configuration.port, address)
        || !configure_socket(socket.value, configuration)) {
        std::cerr << "caller setup failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }
    callback_context.expected_socket = socket.value;
    callback_context.expected_error = configuration.expected_connect_error;
    if (configuration.connect_callback) {
        const bool asynchronous = false;
        if (!set_option(socket.value, SRTO_RCVSYN, asynchronous)
            || srt_connect_callback(socket.value,
                   validate_connect_callback, &callback_context)
                == SRT_ERROR) {
            std::cerr << "connect callback registration failed: "
                      << srt_getlasterror_str() << '\n';
            return 3;
        }
    }
    const int connect_result = srt_connect(socket.value,
        address.data(), address.size);
    if (connect_result == SRT_ERROR) {
        const std::string error{srt_getlasterror_str()};
        const int rejection_reason =
            srt_getrejectreason(socket.value);
        if (configuration.expected_rejection_reason >= 0
            && rejection_reason
                == configuration.expected_rejection_reason) {
            std::cout << "{\"event\":\"rejection_match\","
                         "\"reason\":"
                      << rejection_reason << "}\n" << std::flush;
            return 0;
        }
        std::cerr << "caller setup failed: "
                  << error
                  << "; reject_reason=" << rejection_reason
                  << '\n';
        return 3;
    }
    if (configuration.connect_callback) {
        const auto deadline = Clock::now()
            + std::chrono::milliseconds{
                configuration.timeout_milliseconds + 2'000};
        if (configuration.expected_connect_error != SRT_SUCCESS) {
            if (!wait_for_connect_callback(callback_context, deadline)) {
                std::cerr
                    << "connect callback validation was not completed\n";
                return 3;
            }
            print_connect_callback_observation(callback_context);
            return 0;
        }
        if (!wait_for_connected_socket(
                socket.value, callback_context, deadline)) {
            std::cerr << "asynchronous connection was not completed\n";
            if (callback_context.called.load(
                    std::memory_order_acquire)) {
                print_connect_callback_observation(callback_context);
            }
            return 3;
        }
        if (configuration.require_connect_callback) {
            if (!wait_for_connect_callback(callback_context, deadline)) {
                std::cerr
                    << "connect callback validation was not completed\n";
                return 3;
            }
            print_connect_callback_observation(callback_context);
        }
    }
    if (configuration.expected_rejection_reason >= 0) {
        std::cerr << "caller unexpectedly connected; expected reject_reason="
                  << configuration.expected_rejection_reason << '\n';
        return 3;
    }
    if (!verify_expected_maximum_payload_size(socket.value, configuration)) {
        return 7;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    if (!verify_expected_crypto_mode(socket.value, configuration)) {
        return 7;
    }
#endif
    const int result = transfer_payload(
        socket.value, configuration, role_name(configuration.role));
    if (configuration.connect_callback
        && !configuration.require_connect_callback) {
        print_connect_callback_observation(callback_context);
    }
    return result;
}

int run_rendezvous(const Configuration& configuration)
{
    SocketHandle socket{srt_create_socket()};
    EndpointAddress local_address;
    EndpointAddress peer_address;
    const bool enabled = true;
    if (socket.value == SRT_INVALID_SOCK
        || !make_endpoint(configuration.local_host,
            configuration.local_port, local_address)
        || !make_endpoint(
            configuration.host, configuration.port, peer_address)
        || !configure_socket(socket.value, configuration)
        || !set_option(socket.value, SRTO_RENDEZVOUS, enabled)
        || srt_bind(socket.value,
               local_address.data(), local_address.size)
            == SRT_ERROR) {
        std::cerr << "rendezvous bind setup failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }
    std::cout << "{\"event\":\"ready\",\"role\":\""
              << role_name(configuration.role)
              << "\",\"port\":" << configuration.local_port
              << ",\"srt_version\":" << srt_getversion()
              << "}\n" << std::flush;
    if (srt_connect(socket.value,
            peer_address.data(), peer_address.size)
        == SRT_ERROR) {
        std::cerr << "rendezvous connect failed: "
                  << srt_getlasterror_str() << '\n';
        return 4;
    }
    if (!verify_expected_stream_id(
            socket.value, configuration)) {
        return 7;
    }
    if (!verify_expected_maximum_payload_size(socket.value, configuration)) {
        return 7;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    if (!verify_expected_crypto_mode(socket.value, configuration)) {
        return 7;
    }
#endif
    return transfer_payload(
        socket.value, configuration, role_name(configuration.role));
}

void usage()
{
#ifdef ENABLE_AEAD_API_PREVIEW
    constexpr const char* crypto_mode_usage = " [--crypto-mode auto|ctr|gcm]"
                                              " [--expect-crypto-mode ctr|gcm]";
#else
    constexpr const char* crypto_mode_usage = "";
#endif
    std::cerr << "usage: robotweax_srt_interop_peer"
                 " listener|caller|rendezvous-sender|rendezvous-receiver"
                 " --host IP --port PORT --bytes N"
                 " (--input PATH | --output PATH)"
                 " [--local-host IP --local-port PORT]"
                 " [--data-role sender|receiver]"
                 " [--transport live|file] [--congestion live|file]"
                 " [--message-api true|false]"
                 " [--timeout-ms N] [--peer-idle-timeout-ms N]"
                 " [--chunk-size N] [--receive-size N]"
                 " [--flow-window N] [--send-buffer N] [--mss 76..1500]"
                 " [--ipttl 1..255] [--iptos 0..255]"
                 " [--payload-size 1..1456]"
                 " [--expect-payload-size 1..1456]"
                 " [--loss-max-ttl N]"
                 " [--passphrase-env NAME] [--pbkeylen 16|24|32]"
              << crypto_mode_usage
              << " [--packet-filter CONFIG] [--stream-id ID]"
                 " [--expect-stream-id ID]"
                 " [--listen-callback-stream-id ID]"
                 " [--listen-callback-passphrase-env NAME]"
                 " [--listen-callback-reject N] [--expect-reject N]"
                 " [--connect-callback] [--require-connect-callback]"
                 " [--receive-contract-sender|--receive-contract-receiver]"
                 " [--expect-connect-error N]"
                 " [--latency-ms N]"
                 " [--tlpktdrop on|off]"
                 " [--input-bw N] [--max-bw N] [--second-max-bw N]"
                 " [--km-refresh-rate N] [--km-preannounce N]"
                 " [--shutdown-grace-ms N] [--sender-start-delay-ms N]"
                 " [--source-pacing]"
                 " [--explicit-source-time] [--expect-eof]"
                 " [--expect-peer-shutdown]"
                 " [--expect-peer-idle-timeout]"
                 " [--file-api] [--file-size-to-eof]"
                 " [--expect-peer-error]"
                 " [--expect-file-write-error]"
                 " [--expect-injected-peer-error]"
                 " [--minimum-isn N] [--isn-search-limit N]\n";
}

} // namespace

int main(int argc, char** argv)
{
    Configuration configuration;
    if (!parse_arguments(argc, argv, configuration)) {
        usage();
        return 2;
    }
    if (srt_startup() == SRT_ERROR) {
        std::cerr << "srt_startup failed\n";
        return 2;
    }
#if defined(ROBOTWEAX_SRT_REFERENCE_DIAGNOSTICS)
    srt_setloglevel(LOG_DEBUG);
#endif
    int result = 0;
    switch (configuration.role) {
    case Role::listener:
        result = run_listener(configuration);
        break;
    case Role::caller:
        result = run_caller(configuration);
        break;
    case Role::rendezvous_sender:
    case Role::rendezvous_receiver:
        result = run_rendezvous(configuration);
        break;
    }
    (void)srt_cleanup();
    return result;
}
