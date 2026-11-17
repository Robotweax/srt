// SPDX-License-Identifier: MIT
// Measures a coordinated multicast sender against N loopback receivers in one
// process: achieved send rate of blocking srt_sendmsg calls and how often the
// coordinator refused a send because a member admission was busy, because the
// busy retry gate was still closed, or because the queue or source was full.
// Diagnostic only: loopback timing on one host, not a qualification.
#include "compat/group_registry.hpp"
#include "compat/group_send_coordinator.hpp"
#include "srt/srt.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace robotweax::srt::compat;

sockaddr_in loopback(std::uint16_t port)
{
    sockaddr_in result {};
    result.sin_family = AF_INET;
    result.sin_port = htons(port);
    result.sin_addr.s_addr = htonl(0x7f000001U);
    return result;
}

int admit(void*, SRTSOCKET socket, int, const sockaddr*, const char*)
{
    std::int32_t type = -1;
    int size = sizeof(type);
    if (srt_getsockflag(socket, SRTO_GROUPTYPE, &type, &size) != 0
        || type != static_cast<std::int32_t>(ROBOTWEAX_SRT_GTYPE_MULTICAST))
        return -1;
    const std::uint64_t domain = 7;
    if (srt_setsockflag(
            socket, SRTO_ROBOTWEAX_GROUPDOMAIN, &domain, sizeof(domain))
        != 0)
        return -1;
    const char key[] = "bench-1";
    return srt_setsockflag(socket, SRTO_ROBOTWEAX_MULTICAST, key,
        static_cast<int>(sizeof(key) - 1U));
}

bool require(bool ok, const char* what)
{
    if (!ok)
        std::fprintf(stderr, "%s: %s\n", what, srt_getlasterror_str());
    return ok;
}

std::size_t members(SRTSOCKET group)
{
    (void)GroupRegistry::instance().state(group);
    const auto record = GroupRegistry::instance().find(group);
    if (!record)
        return 0;
    std::lock_guard lock(record->mutex);
    std::size_t connected = 0;
    for (const auto& member : record->members)
        connected += member.public_data.sockstate == SRTS_CONNECTED;
    return connected;
}
} // namespace

int main(int argc, char** argv)
{
    const std::size_t receivers =
        argc > 1 ? static_cast<std::size_t>(std::atoi(argv[1])) : 4;
    const std::size_t messages =
        argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2])) : 2000;
    const std::int64_t rate = argc > 3 ? std::atoll(argv[3]) : 50'000'000;
    const std::size_t payload =
        argc > 4 ? static_cast<std::size_t>(std::atoi(argv[4])) : 1316;
    if (srt_startup() != 0)
        return 2;
    const SRTSOCKET listener = srt_create_socket();
    const bool yes = true;
    auto local = loopback(0);
    const auto multicast_group = [] {
        sockaddr_in g = loopback(26'999);
        g.sin_addr.s_addr = htonl(0xefff850bU); // 239.255.133.11
        return g;
    }();
    const std::int32_t hops = 0;
    bool ok = require(srt_setsockflag(listener,
                          SRTO_ROBOTWEAX_MCAST_INDEPENDENT, &yes, sizeof(yes))
                      == 0,
                  "independent")
        && require(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_COORDINATED,
                       &yes, sizeof(yes))
                == 0,
            "coordinated")
        && require(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_SOURCE,
                       &local, sizeof(local))
                == 0,
            "source")
        && require(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP,
                       &multicast_group, sizeof(multicast_group))
                == 0,
            "group")
        && require(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_IFACE, &local,
                       sizeof(local))
                == 0,
            "iface")
        && require(srt_setsockflag(
                       listener, SRTO_ROBOTWEAX_MCAST_TTL, &hops, sizeof(hops))
                == 0,
            "ttl")
        && require(
            srt_setsockflag(listener, SRTO_MAXBW, &rate, sizeof(rate)) == 0,
            "maxbw")
        && require(
            srt_listen_callback(listener, admit, nullptr) == 0, "callback")
        && require(srt_bind(listener, reinterpret_cast<const sockaddr*>(&local),
                       sizeof(local))
                == 0,
            "bind")
        && require(srt_listen(listener, 64) == 0, "listen");
    if (!ok)
        return 2;
    sockaddr_in bound {};
    int size = sizeof(bound);
    if (!require(srt_getsockname(
                     listener, reinterpret_cast<sockaddr*>(&bound), &size)
                == 0,
            "getsockname"))
        return 2;

    std::vector<SRTSOCKET> groups;
    SRTSOCKET sender = SRT_INVALID_SOCK;
    for (std::size_t i = 0; i < receivers; ++i) {
        const SRTSOCKET group = srt_create_group(ROBOTWEAX_SRT_GTYPE_MULTICAST);
        const std::int32_t latency = 120, receive_timeout = 3'000;
        auto iface = loopback(0);
        ok = require(srt_setsockflag(group, SRTO_ROBOTWEAX_MCAST_INDEPENDENT,
                         &yes, sizeof(yes))
                     == 0,
                 "rx independent")
            && require(
                srt_setsockflag(group, SRTO_LATENCY, &latency, sizeof(latency))
                    == 0,
                "rx latency")
            && require(srt_setsockflag(group, SRTO_RCVTIMEO, &receive_timeout,
                           sizeof(receive_timeout))
                    == 0,
                "rx timeout")
            && require(srt_setsockflag(group, SRTO_ROBOTWEAX_MCAST_IFACE,
                           &iface, sizeof(iface))
                    == 0,
                "rx iface");
        if (!ok)
            return 2;
        SRT_SOCKGROUPCONFIG endpoint = srt_prepare_endpoint(
            nullptr, reinterpret_cast<const sockaddr*>(&bound), sizeof(bound));
        if (!require(
                srt_connect_group(group, &endpoint, 1) != SRT_ERROR, "connect"))
            return 2;
        groups.push_back(group);
        if (sender == SRT_INVALID_SOCK) {
            sender = srt_accept(listener, nullptr, nullptr);
            if (!require(sender != SRT_INVALID_SOCK, "accept"))
                return 2;
        }
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds {10};
    while (members(sender) < receivers
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds {5});
    if (members(sender) < receivers) {
        std::fprintf(stderr, "only %zu of %zu receivers joined\n",
            members(sender), receivers);
        return 2;
    }
    // Receivers drain in their own threads so their buffers never fill.
    std::vector<std::thread> drains;
    std::atomic<bool> stop {false};
    std::atomic<std::uint64_t> received {0};
    for (const auto group : groups)
        drains.emplace_back([group, &stop, &received] {
            std::vector<char> buffer(2048);
            while (!stop.load()) {
                const int n = srt_recvmsg(
                    group, buffer.data(), static_cast<int>(buffer.size()));
                if (n > 0)
                    ++received;
                else if (n == SRT_ERROR
                    && srt_getlasterror(nullptr) != SRT_EASYNCRCV
                    && srt_getlasterror(nullptr) != SRT_ETIMEOUT)
                    break;
            }
        });

    std::shared_ptr<GroupSendCoordinator> coordinator;
    {
        const auto record = GroupRegistry::instance().find(sender);
        std::lock_guard lock(record->mutex);
        coordinator = record->dispatch->coordinator;
    }
    const auto before = coordinator->snapshot();
    std::vector<char> message(payload, 'x');
    std::size_t sent = 0, errors = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < messages; ++i) {
        std::memcpy(message.data(), &i, sizeof(i));
        if (srt_sendmsg(
                sender, message.data(), static_cast<int>(message.size()), -1, 1)
            == static_cast<int>(message.size()))
            ++sent;
        else
            ++errors;
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto after = coordinator->snapshot();
    const double seconds = std::chrono::duration<double>(elapsed).count();
    std::this_thread::sleep_for(std::chrono::milliseconds {400});
    stop = true;
    std::printf("{\"receivers\":%zu,\"messages\":%zu,\"payload_bytes\":%zu,"
                "\"maxbw_bytes_per_second\":%lld,"
                "\"sent\":%zu,\"send_errors\":%zu,\"seconds\":%.3f,\"sends_per_"
                "second\":%.0f,"
                "\"busy_sends\":%llu,\"gated_sends\":%llu,\"full_sends\":%llu,"
                "\"accepted_frames\":%llu,\"detached_members\":%zu,\"received_"
                "messages\":%llu}\n",
        receivers, messages, payload, static_cast<long long>(rate), sent,
        errors, seconds, seconds > 0 ? sent / seconds : 0.0,
        static_cast<unsigned long long>(after.busy_sends - before.busy_sends),
        static_cast<unsigned long long>(after.gated_sends - before.gated_sends),
        static_cast<unsigned long long>(after.full_sends - before.full_sends),
        static_cast<unsigned long long>(
            after.accepted_frames - before.accepted_frames),
        after.detached_members,
        static_cast<unsigned long long>(received.load()));
    for (auto& drain : drains)
        drain.join();
    for (const auto group : groups)
        (void)srt_close(group);
    (void)srt_close(sender);
    (void)srt_close(listener);
    srt_cleanup();
    return errors == 0 ? 0 : 1;
}
