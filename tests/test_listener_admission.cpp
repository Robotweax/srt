#include "test.hpp"
#include "compat/group_registry.hpp"
#include "compat/socket_registry.hpp"
#include "srt/srt.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {
using namespace robotweax::srt::compat;
using namespace std::chrono_literals;

struct AdmissionFixture {
    std::atomic<unsigned> calls {0};
    std::atomic_bool reject_next {false};
    std::vector<SRTSOCKET> sockets;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    sockaddr_in address {};
    bool encrypted = false;
    static constexpr char passphrase[] = "local-admission-test-key";

    AdmissionFixture(bool groups = false, bool encryption = false)
        : encrypted(encryption)
    {
        REQUIRE_EQ(srt_startup(), 0);
        listener = keep(srt_create_socket());
        const int timeout = 300;
        REQUIRE_EQ(srt_setsockflag(
                       listener, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
            0);
        if (groups) {
            const bool enabled = true;
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_GROUPCONNECT, &enabled,
                           sizeof(enabled)),
                0);
        }
        configure_key(listener);
        REQUIRE_EQ(srt_listen_callback(
                       listener,
                       [](void* opaque, SRTSOCKET socket, int, const sockaddr*,
                           const char*) -> int {
                           auto& fixture =
                               *static_cast<AdmissionFixture*>(opaque);
                           ++fixture.calls;
                           if (fixture.reject_next.exchange(false))
                               return -1;
                           std::int32_t group_type = SRT_GTYPE_UNDEFINED;
                           int size = sizeof(group_type);
                           if (srt_getsockflag(
                                   socket, SRTO_GROUPTYPE, &group_type, &size)
                               != 0)
                               return -1;
                           if (group_type != SRT_GTYPE_UNDEFINED) {
                               const std::uint64_t domain = 11;
                               return srt_setsockflag(socket,
                                   SRTO_ROBOTWEAX_GROUPDOMAIN, &domain,
                                   sizeof(domain));
                           }
                           return 0;
                       },
                       this),
            0);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(srt_bind(listener, reinterpret_cast<sockaddr*>(&address),
                       sizeof(address)),
            0);
        int length = sizeof(address);
        REQUIRE_EQ(srt_getsockname(listener,
                       reinterpret_cast<sockaddr*>(&address), &length),
            0);
        REQUIRE_EQ(srt_listen(listener, 1), 0);
    }
    ~AdmissionFixture()
    {
        for (const auto socket : sockets)
            srt_close(socket);
        srt_cleanup();
    }
    SRTSOCKET keep(SRTSOCKET socket)
    {
        REQUIRE(socket != SRT_INVALID_SOCK);
        sockets.push_back(socket);
        return socket;
    }
    void configure_key(SRTSOCKET socket)
    {
        if (encrypted)
            REQUIRE_EQ(srt_setsockflag(socket, SRTO_PASSPHRASE, passphrase,
                           sizeof(passphrase) - 1),
                0);
    }
    SRTSOCKET caller(bool correct_key = true)
    {
        const auto socket = keep(srt_create_socket());
        const int timeout = 300;
        REQUIRE_EQ(
            srt_setsockflag(socket, SRTO_CONNTIMEO, &timeout, sizeof(timeout)),
            0);
        if (correct_key)
            configure_key(socket);
        return socket;
    }
    int connect(SRTSOCKET socket)
    {
        return srt_connect(
            socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }
    template <class Predicate> void wait(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(predicate());
    }
    auto listener_record()
    {
        return SocketRegistry::instance().find(listener);
    }
};
}

TEST(listener_admission_callback_rejection_releases_accept_reservation)
{
    AdmissionFixture fixture;
    fixture.reject_next = true;
    REQUIRE_EQ(fixture.connect(fixture.caller()), SRT_ERROR);
    REQUIRE_EQ(fixture.calls.load(), 1U);
    REQUIRE_EQ(fixture.connect(fixture.caller()), 0);
    fixture.keep(srt_accept(fixture.listener, nullptr, nullptr));
    REQUIRE_EQ(fixture.calls.load(), 2U);
}

TEST(listener_admission_crypto_failure_releases_accept_reservation)
{
    AdmissionFixture fixture {false, true};
    REQUIRE_EQ(fixture.connect(fixture.caller(false)), SRT_ERROR);
    REQUIRE_EQ(fixture.connect(fixture.caller()), 0);
    fixture.keep(srt_accept(fixture.listener, nullptr, nullptr));
    REQUIRE_EQ(fixture.calls.load(), 2U);
}

TEST(listener_admission_full_backlog_preserves_existing_group_members)
{
    AdmissionFixture fixture {true};
    const auto group = fixture.keep(srt_create_group(SRT_GTYPE_BROADCAST));
    auto first = srt_prepare_endpoint(nullptr,
        reinterpret_cast<sockaddr*>(&fixture.address), sizeof(fixture.address));
    REQUIRE(srt_connect_group(group, &first, 1) != SRT_ERROR);
    const auto listener = fixture.listener_record();
    REQUIRE(listener);
    fixture.wait([&] {
        return listener->pending_accepts.load() == 1U;
    });
    auto second = srt_prepare_endpoint(nullptr,
        reinterpret_cast<sockaddr*>(&fixture.address), sizeof(fixture.address));
    REQUIRE(srt_connect_group(group, &second, 1) != SRT_ERROR);
    fixture.wait([&] {
        return srt_getsockstate(second.id) != SRTS_CONNECTING;
    });
    REQUIRE_EQ(srt_getsockstate(second.id), SRTS_CONNECTED);
    REQUIRE_EQ(listener->pending_accepts.load(), 1U);
    const auto mirror =
        fixture.keep(srt_accept(fixture.listener, nullptr, nullptr));
    const auto record = GroupRegistry::instance().find(mirror);
    REQUIRE(record);
    {
        std::lock_guard lock(record->mutex);
        REQUIRE_EQ(record->members.size(), 2U);
    }
    REQUIRE_EQ(fixture.calls.load(), 2U);
    // Additional-member non-publication releases the capacity for a new group.
    const auto next = fixture.keep(srt_create_group(SRT_GTYPE_BROADCAST));
    auto endpoint = srt_prepare_endpoint(nullptr,
        reinterpret_cast<sockaddr*>(&fixture.address), sizeof(fixture.address));
    REQUIRE(srt_connect_group(next, &endpoint, 1) != SRT_ERROR);
    fixture.keep(srt_accept(fixture.listener, nullptr, nullptr));
}

TEST(listener_admission_full_backlog_rejects_first_member_of_another_group)
{
    AdmissionFixture fixture {true};
    const auto first = fixture.keep(srt_create_group(SRT_GTYPE_BROADCAST));
    auto endpoint = srt_prepare_endpoint(nullptr,
        reinterpret_cast<sockaddr*>(&fixture.address), sizeof(fixture.address));
    REQUIRE(srt_connect_group(first, &endpoint, 1) != SRT_ERROR);
    const auto listener = fixture.listener_record();
    REQUIRE(listener);
    fixture.wait([&] {
        return listener->pending_accepts.load() == 1U;
    });
    const auto second = fixture.keep(srt_create_group(SRT_GTYPE_BROADCAST));
    const int timeout = 300;
    REQUIRE_EQ(
        srt_setsockflag(second, SRTO_CONNTIMEO, &timeout, sizeof(timeout)), 0);
    auto excess = srt_prepare_endpoint(nullptr,
        reinterpret_cast<sockaddr*>(&fixture.address), sizeof(fixture.address));
    REQUIRE_EQ(srt_connect_group(second, &excess, 1), SRT_ERROR);
    REQUIRE_EQ(listener->pending_accepts.load(), 1U);
    fixture.keep(srt_accept(fixture.listener, nullptr, nullptr));
    const auto next = fixture.keep(srt_create_group(SRT_GTYPE_BROADCAST));
    auto retry = srt_prepare_endpoint(nullptr,
        reinterpret_cast<sockaddr*>(&fixture.address), sizeof(fixture.address));
    REQUIRE(srt_connect_group(next, &retry, 1) != SRT_ERROR);
    fixture.keep(srt_accept(fixture.listener, nullptr, nullptr));
}
