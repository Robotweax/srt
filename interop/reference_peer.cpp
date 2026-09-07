// Test-only black-box peer built against Haivision's public C API.
#include "srt.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

sockaddr_in endpoint(std::uint16_t port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    (void)inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    return address;
}

int listener_receive(std::uint16_t port, const std::string& expected)
{
    const SRTSOCKET listener = srt_create_socket();
    const auto address = endpoint(port);
    if (listener == SRT_INVALID_SOCK
        || srt_bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SRT_ERROR
        || srt_listen(listener, 1) == SRT_ERROR) {
        std::cerr << srt_getlasterror_str() << '\n';
        return 2;
    }
    sockaddr_in peer{};
    int peer_size = sizeof(peer);
    const SRTSOCKET connected = srt_accept(listener,
        reinterpret_cast<sockaddr*>(&peer), &peer_size);
    if (connected == SRT_INVALID_SOCK) {
        std::cerr << srt_getlasterror_str() << '\n';
        return 2;
    }
    char buffer[4096]{};
    const int received = srt_recvmsg(connected, buffer, sizeof(buffer));
    const bool matched = received == static_cast<int>(expected.size())
        && std::memcmp(buffer, expected.data(), expected.size()) == 0;
    (void)srt_close(connected);
    (void)srt_close(listener);
    if (!matched) {
        std::cerr << "payload mismatch, received " << received << " bytes\n";
        return 3;
    }
    std::cout << "RECEIVED " << expected << '\n';
    return 0;
}

int caller_send(std::uint16_t port, const std::string& message, bool chunked)
{
    const SRTSOCKET socket = srt_create_socket();
    const auto address = endpoint(port);
    const int enabled = 1;
    if (socket == SRT_INVALID_SOCK
        || srt_setsockflag(socket, SRTO_TSBPDMODE,
               &enabled, sizeof(enabled)) == SRT_ERROR
        || srt_connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SRT_ERROR) {
        std::cerr << srt_getlasterror_str() << '\n';
        return 2;
    }
    std::size_t sent = 0;
    while (sent < message.size()) {
        const std::size_t chunk_size = chunked
            ? std::min<std::size_t>(1'000, message.size() - sent)
            : message.size();
        if (srt_sendmsg(socket, message.data() + sent,
                static_cast<int>(chunk_size), -1, 1)
            != static_cast<int>(chunk_size)) {
            std::cerr << srt_getlasterror_str() << '\n';
            return 3;
        }
        sent += chunk_size;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    (void)srt_close(socket);
    std::cout << "SENT " << message << '\n';
    return 0;
}

int listener_measure_rate(std::uint16_t port, std::size_t expected_bytes)
{
    const SRTSOCKET listener = srt_create_socket();
    const auto address = endpoint(port);
    if (listener == SRT_INVALID_SOCK
        || srt_bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SRT_ERROR
        || srt_listen(listener, 1) == SRT_ERROR) {
        return 2;
    }
    sockaddr_in peer{};
    int peer_size = sizeof(peer);
    const SRTSOCKET connected = srt_accept(listener,
        reinterpret_cast<sockaddr*>(&peer), &peer_size);
    if (connected == SRT_INVALID_SOCK) {
        return 2;
    }

    std::array<char, 2'000> buffer{};
    std::size_t received_bytes = 0;
    std::chrono::steady_clock::time_point first_arrival{};
    std::chrono::steady_clock::time_point middle_arrival{};
    std::chrono::steady_clock::time_point last_arrival{};
    while (received_bytes < expected_bytes) {
        const int received = srt_recvmsg(connected, buffer.data(),
            static_cast<int>(buffer.size()));
        if (received <= 0) {
            std::cerr << srt_getlasterror_str() << '\n';
            return 3;
        }
        const auto now = std::chrono::steady_clock::now();
        if (received_bytes == 0U) {
            first_arrival = now;
        }
        last_arrival = now;
        received_bytes += static_cast<std::size_t>(received);
        if (middle_arrival == std::chrono::steady_clock::time_point{}
            && received_bytes >= expected_bytes / 2U) {
            middle_arrival = now;
        }
    }
    const auto elapsed = std::max<std::int64_t>(1,
        std::chrono::duration_cast<std::chrono::microseconds>(
            last_arrival - first_arrival).count());
    const auto rate = received_bytes * 1'000'000ULL
        / static_cast<std::uint64_t>(elapsed);
    const auto first_elapsed = std::max<std::int64_t>(1,
        std::chrono::duration_cast<std::chrono::microseconds>(
            middle_arrival - first_arrival).count());
    const auto second_elapsed = std::max<std::int64_t>(1,
        std::chrono::duration_cast<std::chrono::microseconds>(
            last_arrival - middle_arrival).count());
    const auto first_rate = (expected_bytes / 2U) * 1'000'000ULL
        / static_cast<std::uint64_t>(first_elapsed);
    const auto second_rate = (expected_bytes - expected_bytes / 2U) * 1'000'000ULL
        / static_cast<std::uint64_t>(second_elapsed);
    (void)srt_close(connected);
    (void)srt_close(listener);
    std::cout << "RATE_BPS=" << rate << " RATE1_BPS=" << first_rate
              << " RATE2_BPS=" << second_rate
              << " BYTES=" << received_bytes << '\n';
    return 0;
}

int caller_drop_then_send(std::uint16_t port, const std::string& message)
{
    const SRTSOCKET socket = srt_create_socket();
    const auto address = endpoint(port);
    const int enabled = 1;
    const int drop_delay = 0;
    std::int64_t maximum_bandwidth = 1'000;
    if (socket == SRT_INVALID_SOCK
        || srt_setsockflag(socket, SRTO_TSBPDMODE,
               &enabled, sizeof(enabled)) == SRT_ERROR
        || srt_setsockflag(socket, SRTO_TLPKTDROP,
               &enabled, sizeof(enabled)) == SRT_ERROR
        || srt_setsockflag(socket, SRTO_SNDDROPDELAY,
               &drop_delay, sizeof(drop_delay)) == SRT_ERROR
        || srt_setsockflag(socket, SRTO_MAXBW,
               &maximum_bandwidth, sizeof(maximum_bandwidth)) == SRT_ERROR
        || srt_connect(socket, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == SRT_ERROR) {
        std::cerr << srt_getlasterror_str() << '\n';
        return 2;
    }

    const std::string expired(1'000, 'x');
    for (int index = 0; index < 100; ++index) {
        if (srt_sendmsg(socket, expired.data(), static_cast<int>(expired.size()),
                -1, 1) != static_cast<int>(expired.size())) {
            std::cerr << srt_getlasterror_str() << '\n';
            return 3;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1'600});
    maximum_bandwidth = 10'000'000;
    if (srt_setsockflag(socket, SRTO_MAXBW,
            &maximum_bandwidth, sizeof(maximum_bandwidth)) == SRT_ERROR
        || srt_sendmsg(socket, message.data(), static_cast<int>(message.size()),
               -1, 1) != static_cast<int>(message.size())) {
        std::cerr << srt_getlasterror_str() << '\n';
        return 3;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{3'000});
    (void)srt_close(socket);
    std::cout << "DROP_THEN_SENT " << message << '\n';
    return 0;
}

int caller_clock_test(std::uint16_t port, std::size_t message_count)
{
    const SRTSOCKET socket = srt_create_socket();
    const auto address = endpoint(port);
    const int enabled = 1;
    if (socket == SRT_INVALID_SOCK
        || srt_setsockflag(socket, SRTO_TSBPDMODE,
               &enabled, sizeof(enabled)) == SRT_ERROR
        || srt_connect(socket, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == SRT_ERROR) {
        std::cerr << srt_getlasterror_str() << '\n';
        return 2;
    }
    const char byte = 'd';
    for (std::size_t index = 0; index < message_count; ++index) {
        if (srt_sendmsg(socket, &byte, 1, -1, 1) != 1) {
            std::cerr << srt_getlasterror_str() << '\n';
            return 3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    (void)srt_close(socket);
    std::cout << "CLOCK_MESSAGES=" << message_count << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr << "usage: reference_peer MODE PORT VALUE\n";
        return 1;
    }
    const auto port_value = std::strtoul(argv[2], nullptr, 10);
    if (port_value == 0 || port_value > 65'535) {
        return 1;
    }
    if (srt_startup() == SRT_ERROR) {
        return 2;
    }
    const std::string mode{argv[1]};
    const int result = mode == "caller-send" || mode == "caller-send-chunks"
        ? caller_send(static_cast<std::uint16_t>(port_value), argv[3],
              mode == "caller-send-chunks")
        : (mode == "listener-receive"
              ? listener_receive(static_cast<std::uint16_t>(port_value), argv[3])
              : (mode == "listener-measure-rate"
                      ? listener_measure_rate(static_cast<std::uint16_t>(port_value),
                            std::strtoull(argv[3], nullptr, 10))
                      : (mode == "caller-drop-then-send"
                              ? caller_drop_then_send(
                                    static_cast<std::uint16_t>(port_value), argv[3])
                              : (mode == "caller-clock-test"
                                      ? caller_clock_test(
                                            static_cast<std::uint16_t>(port_value),
                                            std::strtoull(argv[3], nullptr, 10))
                                      : 1))));
    (void)srt_cleanup();
    return result;
}
