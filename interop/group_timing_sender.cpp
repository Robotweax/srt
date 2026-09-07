// Public-API Backup-group sender with an explicit continuous source timeline.

#include "srt.h"

#include <algorithm>
#include <array>
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
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Configuration {
    std::string host = "127.0.0.1";
    std::uint16_t primary_port = 0;
    std::uint16_t backup_port = 0;
    std::string input_path;
    std::size_t message_count = 0;
    int message_size = 0;
    std::uint64_t input_bandwidth = 0;
    std::size_t failover_after = 0;
    bool undrained_failover = false;
    bool external_path_outage = false;
    std::string passphrase_environment;
    int pbkeylen = 0;
    int key_refresh_rate = 0;
    int key_preannouncement = 0;
    int crypto_mode = -1;
    std::int32_t minimum_stability_milliseconds = 60;
    int peer_idle_timeout_milliseconds = 0;
    int timeout_milliseconds = 10'000;
    int shutdown_grace_milliseconds = 250;
};

struct ConfigHandle {
    SRT_SOCKOPT_CONFIG* value = nullptr;

    void reset() noexcept
    {
        srt_delete_config(value);
        value = nullptr;
    }

    ~ConfigHandle()
    {
        reset();
    }
};

struct SocketHandle {
    SRTSOCKET value = SRT_INVALID_SOCK;

    ~SocketHandle()
    {
        if (value != SRT_INVALID_SOCK) {
            (void)srt_close(value);
        }
    }
};

struct SendObservation {
    std::size_t message_index = 0;
    std::int64_t source_time_microseconds = 0;
    SRTSOCKET running_member = SRT_INVALID_SOCK;
    std::size_t member_count = 0;
};

struct SendDrainResult {
    bool drained = false;
    bool query_failed = false;
    std::size_t remaining_blocks = 0;
    std::size_t remaining_bytes = 0;
};

template <class Integer>
bool parse_integer(std::string_view text, Integer& value)
{
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size();
}

bool take_value(int& index, int argc, char** argv, std::string_view& value)
{
    if (index + 1 >= argc) {
        return false;
    }
    value = argv[++index];
    return true;
}

bool parse_port(std::string_view text, std::uint16_t& port)
{
    unsigned parsed = 0;
    if (!parse_integer(text, parsed) || parsed == 0U || parsed > 65'535U) {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

bool parse_arguments(int argc, char** argv, Configuration& configuration)
{
    for (int index = 1; index < argc; ++index) {
        const std::string_view option {argv[index]};
        std::string_view value;
        if (option == "--host" && take_value(index, argc, argv, value)) {
            configuration.host = value;
        } else if (option == "--primary-port"
            && take_value(index, argc, argv, value)) {
            if (!parse_port(value, configuration.primary_port)) {
                return false;
            }
        } else if (option == "--backup-port"
            && take_value(index, argc, argv, value)) {
            if (!parse_port(value, configuration.backup_port)) {
                return false;
            }
        } else if (option == "--input"
            && take_value(index, argc, argv, value)) {
            configuration.input_path = value;
        } else if (option == "--messages"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.message_count)
                || configuration.message_count < 2U
                || configuration.message_count > 1'000'000U) {
                return false;
            }
        } else if (option == "--message-size"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.message_size)
                || configuration.message_size <= 0
                || configuration.message_size > 1'452) {
                return false;
            }
        } else if (option == "--input-bw"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.input_bandwidth)
                || configuration.input_bandwidth == 0U) {
                return false;
            }
        } else if (option == "--failover-after"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.failover_after)) {
                return false;
            }
        } else if (option == "--undrained-failover") {
            configuration.undrained_failover = true;
        } else if (option == "--external-path-outage") {
            configuration.external_path_outage = true;
        } else if (option == "--passphrase-env"
            && take_value(index, argc, argv, value)) {
            configuration.passphrase_environment = value;
        } else if (option == "--pbkeylen"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.pbkeylen)
                || (configuration.pbkeylen != 16 && configuration.pbkeylen != 24
                    && configuration.pbkeylen != 32)) {
                return false;
            }
        } else if (option == "--km-refresh-rate"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.key_refresh_rate)
                || configuration.key_refresh_rate < 2) {
                return false;
            }
        } else if (option == "--km-preannounce"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.key_preannouncement)
                || configuration.key_preannouncement <= 0) {
                return false;
            }
#ifdef ENABLE_AEAD_API_PREVIEW
        } else if (option == "--crypto-mode"
            && take_value(index, argc, argv, value)) {
            if (value == "ctr") {
                configuration.crypto_mode = 1;
            } else if (value == "gcm") {
                configuration.crypto_mode = 2;
            } else {
                return false;
            }
#endif
        } else if (option == "--minimum-stability-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(
                    value, configuration.minimum_stability_milliseconds)) {
                return false;
            }
            if (configuration.minimum_stability_milliseconds < 60
                || configuration.minimum_stability_milliseconds > 5'000) {
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
                || configuration.peer_idle_timeout_milliseconds < 100
                || configuration.peer_idle_timeout_milliseconds > 60'000) {
                return false;
            }
        } else if (option == "--shutdown-grace-ms"
            && take_value(index, argc, argv, value)) {
            if (!parse_integer(value, configuration.shutdown_grace_milliseconds)
                || configuration.shutdown_grace_milliseconds < 0
                || configuration.shutdown_grace_milliseconds > 60'000) {
                return false;
            }
        } else {
            return false;
        }
    }
    const bool encrypted = !configuration.passphrase_environment.empty();
    const bool security_valid = encrypted
        ? configuration.pbkeylen != 0 && configuration.key_refresh_rate >= 2
            && configuration.key_preannouncement > 0
            && configuration.key_preannouncement
                <= (configuration.key_refresh_rate - 1) / 2
        : configuration.pbkeylen == 0 && configuration.key_refresh_rate == 0
            && configuration.key_preannouncement == 0
            && configuration.crypto_mode == -1;
    return configuration.primary_port != 0 && configuration.backup_port != 0
        && !configuration.input_path.empty()
        && configuration.message_count != 0U && configuration.message_size != 0
        && configuration.input_bandwidth != 0U
        && configuration.failover_after != 0U
        && configuration.failover_after < configuration.message_count
        && !(configuration.undrained_failover
            && configuration.external_path_outage)
        && (configuration.external_path_outage
            == (configuration.peer_idle_timeout_milliseconds != 0))
        && security_valid;
}

bool make_address(const std::string& host, std::uint16_t port,
    sockaddr_storage& storage, int& size)
{
    sockaddr_in ipv4 {};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &ipv4.sin_addr) == 1) {
        std::memcpy(&storage, &ipv4, sizeof(ipv4));
        size = static_cast<int>(sizeof(ipv4));
        return true;
    }

    sockaddr_in6 ipv6 {};
    ipv6.sin6_family = AF_INET6;
    ipv6.sin6_port = htons(port);
    if (inet_pton(AF_INET6, host.c_str(), &ipv6.sin6_addr) == 1) {
        std::memcpy(&storage, &ipv6, sizeof(ipv6));
        size = static_cast<int>(sizeof(ipv6));
        return true;
    }
    return false;
}

template <class Value>
bool set_option(SRTSOCKET socket, SRT_SOCKOPT option, const Value& value)
{
    return srt_setsockflag(
               socket, option, &value, static_cast<int>(sizeof(value)))
        != SRT_ERROR;
}

bool set_string_option(SRTSOCKET socket, SRT_SOCKOPT option, const char* value)
{
    const auto size = std::char_traits<char>::length(value);
    return size <= static_cast<std::size_t>(std::numeric_limits<int>::max())
        && srt_setsockflag(socket, option, value, static_cast<int>(size))
        != SRT_ERROR;
}

#ifdef ENABLE_AEAD_API_PREVIEW
bool read_integer_option(SRTSOCKET socket, SRT_SOCKOPT option, int& value)
{
    int size = static_cast<int>(sizeof(value));
    return srt_getsockflag(socket, option, &value, &size) != SRT_ERROR
        && size == static_cast<int>(sizeof(value));
}
#endif

template <class Value>
bool add_config_option(
    SRT_SOCKOPT_CONFIG* config, SRT_SOCKOPT option, const Value& value)
{
    return srt_config_add(
               config, option, &value, static_cast<int>(sizeof(value)))
        != SRT_ERROR;
}

bool wait_for_members(SRTSOCKET group, SRTSOCKET primary, SRTSOCKET backup)
{
    const auto deadline = Clock::now() + std::chrono::seconds {5};
    while (Clock::now() < deadline) {
        std::array<SRT_SOCKGROUPDATA, 2> members {};
        std::size_t size = members.size();
        if (srt_group_data(group, members.data(), &size) != SRT_ERROR
            && size == members.size()
            && std::count_if(members.begin(), members.end(),
                   [primary](const SRT_SOCKGROUPDATA& member) {
                       return member.id == primary;
                   })
                == 1
            && std::count_if(members.begin(), members.end(),
                   [backup](const SRT_SOCKGROUPDATA& member) {
                       return member.id == backup;
                   })
                == 1
            && std::all_of(members.begin(), members.end(),
                [primary, backup](const SRT_SOCKGROUPDATA& member) {
                    return (member.id == primary || member.id == backup)
                        && member.sockstate == SRTS_CONNECTED;
                })) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    return false;
}

SendDrainResult wait_for_send_drain(SRTSOCKET socket, int timeout_milliseconds)
{
    SendDrainResult result;
    const auto deadline =
        Clock::now() + std::chrono::milliseconds {timeout_milliseconds};
    while (Clock::now() < deadline) {
        if (srt_getsndbuffer(
                socket, &result.remaining_blocks, &result.remaining_bytes)
            == SRT_ERROR) {
            result.query_failed = true;
            return result;
        }
        if (result.remaining_blocks == 0U && result.remaining_bytes == 0U) {
            result.drained = true;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    return result;
}

SRTSOCKET running_member(const SRT_SOCKGROUPDATA* members, std::size_t size)
{
    SRTSOCKET running = SRT_INVALID_SOCK;
    for (std::size_t index = 0; index < size; ++index) {
        if (members[index].memberstate != SRT_GST_RUNNING) {
            continue;
        }
        if (running != SRT_INVALID_SOCK
            || members[index].id == SRT_INVALID_SOCK) {
            return SRT_INVALID_SOCK;
        }
        running = members[index].id;
    }
    return running;
}

bool member_is_running(
    const SRT_SOCKGROUPDATA* members, std::size_t size, SRTSOCKET member)
{
    return std::count_if(members, members + size,
               [member](const SRT_SOCKGROUPDATA& candidate) {
                   return candidate.id == member
                       && candidate.memberstate == SRT_GST_RUNNING;
               })
        == 1;
}

int run(const Configuration& configuration)
{
    sockaddr_storage primary_address {};
    sockaddr_storage backup_address {};
    int primary_size = 0;
    int backup_size = 0;
    if (!make_address(configuration.host, configuration.primary_port,
            primary_address, primary_size)
        || !make_address(configuration.host, configuration.backup_port,
            backup_address, backup_size)) {
        std::cerr << "invalid group timing endpoint\n";
        return 3;
    }

    const bool encrypted = !configuration.passphrase_environment.empty();
    const char* passphrase = encrypted
        ? std::getenv(configuration.passphrase_environment.c_str())
        : nullptr;
    std::ifstream input {configuration.input_path, std::ios::binary};
    SocketHandle group {srt_create_group(SRT_GTYPE_BACKUP)};
    ConfigHandle member_config {encrypted ? srt_create_config() : nullptr};
    const bool enforced_encryption = true;
    if (!input || group.value == SRT_INVALID_SOCK) {
        std::cerr << "group timing sender setup failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }
    bool crypto_mode_configured = true;
#ifdef ENABLE_AEAD_API_PREVIEW
    if (configuration.crypto_mode >= 0) {
        crypto_mode_configured =
            set_option(group.value, SRTO_CRYPTOMODE, configuration.crypto_mode);
    }
#endif
    if (encrypted
        && (passphrase == nullptr || *passphrase == '\0'
            || member_config.value == nullptr || !crypto_mode_configured
            || !set_string_option(group.value, SRTO_PASSPHRASE, passphrase)
            || !set_option(group.value, SRTO_PBKEYLEN, configuration.pbkeylen)
            || !set_option(
                group.value, SRTO_ENFORCEDENCRYPTION, enforced_encryption)
            || !add_config_option(member_config.value, SRTO_KMREFRESHRATE,
                configuration.key_refresh_rate)
            || !add_config_option(member_config.value, SRTO_KMPREANNOUNCE,
                configuration.key_preannouncement))) {
        std::cerr << "group timing security setup failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }

    if (!set_option(
            group.value, SRTO_SNDTIMEO, configuration.timeout_milliseconds)
        || !set_option(group.value, SRTO_GROUPMINSTABLETIMEO,
            configuration.minimum_stability_milliseconds)
        || (configuration.peer_idle_timeout_milliseconds != 0
            && !set_option(group.value, SRTO_PEERIDLETIMEO,
                configuration.peer_idle_timeout_milliseconds))) {
        std::cerr << "group timing sender setup failed: "
                  << srt_getlasterror_str() << '\n';
        return 3;
    }

    std::array<SRT_SOCKGROUPCONFIG, 2> endpoints {
        srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&primary_address), primary_size),
        srt_prepare_endpoint(nullptr,
            reinterpret_cast<const sockaddr*>(&backup_address), backup_size),
    };
    endpoints[0].config = member_config.value;
    endpoints[1].config = member_config.value;
    endpoints[0].weight = 20U;
    endpoints[1].weight = 10U;
    endpoints[0].token = 5'001;
    endpoints[1].token = 5'002;
    // Establish the preferred path before adding its standby. Connecting both
    // asynchronously in one call would make the first active member depend on
    // handshake completion order rather than the intended initial policy.
    const SRTSOCKET first = srt_connect_group(group.value, endpoints.data(), 1);
    const SRTSOCKET primary = endpoints[0].id;
    if (first == SRT_INVALID_SOCK || first != primary
        || primary == SRT_INVALID_SOCK
        || srt_getsockstate(primary) != SRTS_CONNECTED) {
        std::cerr << "group timing primary connect failed: "
                  << srt_getlasterror_str() << '\n';
        return 4;
    }

    const SRTSOCKET second =
        srt_connect_group(group.value, endpoints.data() + 1, 1);
    const SRTSOCKET backup = endpoints[1].id;
    if (second == SRT_INVALID_SOCK || second != backup
        || backup == SRT_INVALID_SOCK || primary == backup
        || !wait_for_members(group.value, primary, backup)) {
        std::cerr << "group timing backup connect failed: "
                  << srt_getlasterror_str() << '\n';
        return 4;
    }
#ifdef ENABLE_AEAD_API_PREVIEW
    if (configuration.crypto_mode >= 0) {
        int primary_mode = -1;
        int backup_mode = -1;
        if (!read_integer_option(primary, SRTO_CRYPTOMODE, primary_mode)
            || !read_integer_option(backup, SRTO_CRYPTOMODE, backup_mode)
            || primary_mode != configuration.crypto_mode
            || backup_mode != configuration.crypto_mode) {
            std::cerr << "group timing member crypto-mode mismatch\n";
            return 4;
        }
    }
#endif
    // Both members copied the member-specific pre-connect settings. Release
    // that option object before traffic starts. Security credentials are
    // group-wide options in the Haivision public API and stay owned by the
    // group for the lifetime of its connections.
    member_config.reset();

    const auto started = Clock::now();
    const std::int64_t source_origin = srt_time_now();
    std::cout << "{\"event\":\"source_timeline\","
                 "\"role\":\"group-timing-sender\","
                 "\"origin_microseconds\":"
              << source_origin
              << ",\"bytes_per_second\":" << configuration.input_bandwidth
              << "}\n"
              << "{\"event\":\"group_connected\","
                 "\"primary_member\":"
              << primary << ",\"backup_member\":" << backup << "}\n"
              << std::flush;

    const auto source_offset_at = [&](std::size_t message_index) {
        const auto byte_offset = static_cast<std::uint64_t>(message_index)
            * static_cast<std::uint64_t>(configuration.message_size);
        return static_cast<std::int64_t>(
            byte_offset * 1'000'000U / configuration.input_bandwidth);
    };
    const auto source_time_at = [&](std::size_t message_index) {
        return source_origin + source_offset_at(message_index);
    };

    std::vector<char> payload(
        static_cast<std::size_t>(configuration.message_size));
    std::vector<SendObservation> observations;
    observations.reserve(configuration.message_count);
    bool primary_closed = false;
    std::size_t failover_transition = configuration.message_count;
    for (std::size_t index = 0; index < configuration.message_count; ++index) {
        if (index == configuration.failover_after
            && !configuration.external_path_outage) {
            if (!configuration.undrained_failover) {
                const SendDrainResult drain = wait_for_send_drain(
                    primary, configuration.timeout_milliseconds);
                if (!drain.drained) {
                    std::cerr << "group timing primary did not drain: blocks="
                              << drain.remaining_blocks
                              << "; bytes=" << drain.remaining_bytes
                              << "; query_failed=" << drain.query_failed
                              << "; error=" << srt_getlasterror_str() << '\n';
                    return 5;
                }
            }
            if (srt_close(primary) == SRT_ERROR) {
                std::cerr << "group timing primary close failed: "
                          << srt_getlasterror_str() << '\n';
                return 5;
            }
            primary_closed = true;
        }

        const auto source_elapsed =
            std::chrono::microseconds {source_offset_at(index)};
        std::this_thread::sleep_until(started + source_elapsed);
        input.read(payload.data(), configuration.message_size);
        if (input.gcount() != configuration.message_size) {
            std::cerr << "group timing input ended at message " << index
                      << '\n';
            return 5;
        }

        std::array<SRT_SOCKGROUPDATA, 2> members {};
        SRT_MSGCTRL control = srt_msgctrl_default;
        control.inorder = 1;
        control.srctime = source_time_at(index);
        control.grpdata = members.data();
        control.grpdata_size = members.size();
        const int sent = srt_sendmsg2(
            group.value, payload.data(), configuration.message_size, &control);
        const bool primary_running = control.grpdata != nullptr
            && member_is_running(
                control.grpdata, control.grpdata_size, primary);
        const bool backup_running = control.grpdata != nullptr
            && member_is_running(control.grpdata, control.grpdata_size, backup);
        SRTSOCKET active = control.grpdata == nullptr
            ? SRT_INVALID_SOCK
            : running_member(control.grpdata, control.grpdata_size);
        if (configuration.external_path_outage
            && failover_transition == configuration.message_count
            && index >= configuration.failover_after && backup_running) {
            failover_transition = index;
        }
        if (configuration.external_path_outage) {
            // Once the standby has taken over, a Backup group may probe the
            // higher-weight but externally black-holed primary in parallel.
            // Both members then legitimately report RUNNING for this send.
            // The already-proven replacement remains authoritative until a
            // response-qualified promotion, which the black hole forbids.
            active = failover_transition == configuration.message_count
                ? (primary_running ? primary : SRT_INVALID_SOCK)
                : (backup_running ? backup : SRT_INVALID_SOCK);
        }
        const SRTSOCKET expected = configuration.external_path_outage
            ? (failover_transition == configuration.message_count ? primary
                                                                  : backup)
            : (index < configuration.failover_after ? primary : backup);
        if (sent != configuration.message_size
            || control.srctime != source_time_at(index) || control.pktseq < 0
            || control.msgno <= 0 || control.grpdata == nullptr
            || control.grpdata_size == 0U
            || control.grpdata_size > members.size()
            || !member_is_running(
                control.grpdata, control.grpdata_size, expected)
            || active != expected) {
            std::cerr << "group timing send " << index
                      << " failed: " << srt_getlasterror_str()
                      << "; active=" << active << "; expected=" << expected
                      << "; members=";
            for (std::size_t member_index = 0;
                member_index < control.grpdata_size; ++member_index) {
                const auto& member = control.grpdata[member_index];
                std::cerr << (member_index == 0 ? "[" : ",")
                          << "{id=" << member.id
                          << ",state=" << member.memberstate
                          << ",result=" << member.result << "}";
            }
            std::cerr << "]\n";
            return 5;
        }
        observations.push_back(
            {index, control.srctime, active, control.grpdata_size});
    }

    if ((!configuration.external_path_outage && !primary_closed)
        || (configuration.external_path_outage
            && (primary_closed
                || failover_transition == configuration.message_count
                || failover_transition <= configuration.failover_after))) {
        std::cerr << "group timing failover state was incomplete\n";
        return 6;
    }
    const SendDrainResult backup_drain =
        wait_for_send_drain(backup, configuration.timeout_milliseconds);
    if (!backup_drain.drained) {
        std::cerr << "group timing backup did not drain: blocks="
                  << backup_drain.remaining_blocks
                  << "; bytes=" << backup_drain.remaining_bytes
                  << "; query_failed=" << backup_drain.query_failed
                  << "; error=" << srt_getlasterror_str() << '\n';
        return 6;
    }
    if (configuration.shutdown_grace_milliseconds != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds {
            configuration.shutdown_grace_milliseconds});
    }

    for (const auto& observation : observations) {
        std::cout << "{\"event\":\"group_send\",\"message_index\":"
                  << observation.message_index
                  << ",\"source_time_microseconds\":"
                  << observation.source_time_microseconds
                  << ",\"running_member\":" << observation.running_member
                  << ",\"member_count\":" << observation.member_count << "}\n";
    }
    if (configuration.external_path_outage) {
        std::cout << "{\"event\":\"group_failover\","
                     "\"after_messages\":"
                  << failover_transition << ",\"outage_after_messages\":"
                  << configuration.failover_after
                  << ",\"closed_member\":" << primary
                  << ",\"replacement_member\":" << backup
                  << ",\"failure_injection\":\"external-path-outage\","
                     "\"primary_closed\":false,\"primary_drained\":false}\n"
                  << "{\"event\":\"complete\","
                     "\"role\":\"group-timing-sender\",\"bytes\":"
                  << static_cast<std::uint64_t>(configuration.message_count)
                * static_cast<std::uint64_t>(configuration.message_size)
                  << ",\"messages\":" << configuration.message_count
                  << ",\"primary_closed\":false,\"primary_drained\":false,"
                     "\"external_path_outage\":true}\n"
                  << std::flush;
    } else {
        std::cout << "{\"event\":\"group_failover\","
                     "\"after_messages\":"
                  << configuration.failover_after
                  << ",\"closed_member\":" << primary
                  << ",\"replacement_member\":" << backup
                  << ",\"primary_drained\":"
                  << (configuration.undrained_failover ? "false" : "true")
                  << "}\n"
                  << "{\"event\":\"complete\","
                     "\"role\":\"group-timing-sender\",\"bytes\":"
                  << static_cast<std::uint64_t>(configuration.message_count)
                * static_cast<std::uint64_t>(configuration.message_size)
                  << ",\"messages\":" << configuration.message_count
                  << ",\"primary_closed\":true,\"primary_drained\":"
                  << (configuration.undrained_failover ? "false" : "true")
                  << "}\n"
                  << std::flush;
    }
    return 0;
}

void usage()
{
    std::cerr << "usage: robotweax_srt_group_timing_sender"
                 " --host IP --primary-port PORT --backup-port PORT"
                 " --input PATH --messages N --message-size N"
                 " --input-bw BYTES_PER_SECOND --failover-after N"
                 " [--undrained-failover|--external-path-outage]"
                 " [--passphrase-env NAME --pbkeylen 16|24|32"
                 " --km-refresh-rate N --km-preannounce N]"
#ifdef ENABLE_AEAD_API_PREVIEW
                 " [--crypto-mode ctr|gcm]"
#endif
                 " [--minimum-stability-ms N] [--timeout-ms N]"
                 " [--peer-idle-timeout-ms N]"
                 " [--shutdown-grace-ms N]\n";
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
    const int result = run(configuration);
    (void)srt_cleanup();
    return result;
}
