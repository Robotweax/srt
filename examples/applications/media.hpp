/* SPDX-License-Identifier: MIT */
#pragma once
#include "common.hpp"
#include <csignal>
#include <fstream>
#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>
#endif

namespace samples {
inline volatile std::sig_atomic_t media_stopping = 0;
inline void stop_media(int)
{
    media_stopping = 1;
}

class DatagramSocket {
public:
    SYSSOCKET id = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DatagramSocket()
    {
        if (id == SYSSOCKET_INVALID)
            throw std::runtime_error("cannot create media UDP socket");
    }
    ~DatagramSocket()
    {
#ifdef _WIN32
        (void)closesocket(id);
#else
        (void)::close(id);
#endif
    }
    DatagramSocket(const DatagramSocket&) = delete;
    DatagramSocket& operator=(const DatagramSocket&) = delete;
};

inline void check_ts(const char* bytes, int size)
{
    if (size <= 0 || size > 1316 || size % 188 != 0)
        throw std::runtime_error(
            "media datagram must contain 1..7 complete 188-byte TS packets");
    for (int i = 0; i < size; i += 188)
        if (static_cast<unsigned char>(bytes[i]) != 0x47)
            throw std::runtime_error("media input is not synchronized MPEG-TS");
}

inline void media_stream(SRTSOCKET group, const Arguments& args, bool sender,
    const std::function<void(int)>& hook = {},
    const std::function<void()>& report_hook = {})
{
    const int seconds = args.integer("--duration", 20, 3, 3600);
    DatagramSocket udp;
    const auto endpoint = address(args.get("--media-udp"));
    if (sender
        && ::bind(udp.id, reinterpret_cast<const sockaddr*>(&endpoint),
               sizeof(endpoint))
            != 0)
        throw std::runtime_error("cannot bind media input UDP endpoint");
    std::ofstream recording;
    if (!args.get("--record").empty()) {
        if (sender)
            throw std::runtime_error("--record is receive-only");
        // The caller explicitly chooses the output path; never derive it from a peer.
        recording.open(
            args.get("--record"), std::ios::binary | std::ios::trunc);
        if (!recording)
            throw std::runtime_error("cannot open media recording");
    }
    std::signal(SIGINT, stop_media);
    std::signal(SIGTERM, stop_media);
    const auto started = Clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);
    auto report = started;
    auto last = Clock::time_point {};
    double maximum_gap_ms = 0;
    std::uint64_t bytes_total = 0;
    int datagrams = 0;
    event("\"event\":\"media_ready\",\"role\":\""
        + std::string(sender ? "send" : "receive") + "\"");
    while (!media_stopping && Clock::now() < deadline) {
        std::array<char, 1317> bytes {};
        int size = 0;
        if (sender) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(udp.id, &readable);
            timeval timeout {0, 100000};
            const int ready = ::select(static_cast<int>(udp.id + 1), &readable,
                nullptr, nullptr, &timeout);
            if (ready < 0) {
                if (media_stopping)
                    break;
                throw std::runtime_error("media input select failed");
            }
            if (ready > 0) {
                size = static_cast<int>(::recvfrom(udp.id, bytes.data(),
                    static_cast<int>(bytes.size()), 0, nullptr, nullptr));
                check_ts(bytes.data(), size);
                if (hook)
                    hook(datagrams);
                auto control = srt_msgctrl_default;
                if (srt_sendmsg2(group, bytes.data(), size, &control) != size)
                    fail("send media");
            }
        } else {
            auto control = srt_msgctrl_default;
            size = srt_recvmsg2(
                group, bytes.data(), static_cast<int>(bytes.size()), &control);
            if (size == SRT_ERROR) {
                const int error = srt_getlasterror(nullptr);
                if (error != SRT_ETIMEOUT && error != SRT_EASYNCRCV)
                    fail("receive media");
                size = 0;
            } else {
                check_ts(bytes.data(), size);
                if (::sendto(udp.id, bytes.data(), size, 0,
                        reinterpret_cast<const sockaddr*>(&endpoint),
                        sizeof(endpoint))
                    != size)
                    throw std::runtime_error("media UDP output failed");
                if (recording.is_open()) {
                    recording.write(bytes.data(), size);
                    if (!recording)
                        throw std::runtime_error(
                            "media recording write failed");
                }
            }
        }
        if (size > 0) {
            ++datagrams;
            bytes_total += static_cast<std::uint64_t>(size);
            if (last != Clock::time_point {})
                maximum_gap_ms = std::max(maximum_gap_ms,
                    std::chrono::duration<double, std::milli>(
                        Clock::now() - last)
                        .count());
            last = Clock::now();
        }
        if (Clock::now() >= report) {
            stats(group, "media");
            if (report_hook)
                report_hook();
            event("\"event\":\"media_progress\",\"bytes\":"
                + std::to_string(bytes_total)
                + ",\"datagrams\":" + std::to_string(datagrams));
            report = Clock::now() + 1s;
        }
    }
    if (recording.is_open()) {
        recording.flush();
        if (!recording)
            throw std::runtime_error("media recording flush failed");
    }
    if (bytes_total == 0 && !media_stopping)
        throw std::runtime_error("no MPEG-TS traffic arrived before deadline");
    event("\"event\":\"media_summary\",\"bytes\":" + std::to_string(bytes_total)
        + ",\"datagrams\":" + std::to_string(datagrams)
        + ",\"max_receive_gap_ms\":" + std::to_string(maximum_gap_ms)
        + ",\"stopped\":" + std::string(media_stopping ? "true" : "false"));
}
} // namespace samples
