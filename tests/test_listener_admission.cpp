#include "test.hpp"
#include "compat/group_registry.hpp"
#include "compat/connection.hpp"
#include "compat/listener_handshake_sources.hpp"
#include "compat/listener_connection_setup_sources.hpp"
#include "compat/error_state.hpp"
#include "compat/socket_registry.hpp"
#include "srt/srt.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace robotweax::srt::compat {
// Drive the production post-cookie boundary and consume its real actor results
// explicitly, so admission budgets can use a frozen clock without sleeping.
struct ListenerRuntimeTestAccess {
    static bool pause_results(ListenerRuntime& runtime)
    {
        std::lock_guard lock(runtime.mutex_);
        if (runtime.result_work_active_ || runtime.result_work_scheduled_
            || runtime.setup_context_)
            return false;
        runtime.result_work_scheduled_ = true;
        return true;
    }
    static void resume_results(ListenerRuntime& runtime)
    {
        std::lock_guard lock(runtime.mutex_);
        runtime.result_work_scheduled_ = false;
        runtime.result_work_pending_ = false;
    }
    static SRTSOCKET admit(ListenerRuntime& runtime,
        const std::shared_ptr<SocketRecord>& listener,
        const ListenerHandshakeAdmission& admission,
        std::chrono::steady_clock::time_point time)
    {
        return runtime.start_admitted_socket(listener, admission, time);
    }
    static auto actor(ListenerRuntime& runtime)
    {
        std::lock_guard lock(runtime.mutex_);
        return runtime.setup_actor_;
    }
    static SRT_ERRNO complete(ListenerRuntime& runtime,
        const ListenerConnectionSetupActorResult& result)
    {
        auto context = [&] {
            std::lock_guard lock(runtime.mutex_);
            return runtime.setup_context_;
        }();
        REQUIRE(context);
        clear_last_error();
        (void)runtime.process_setup_result(context, result);
        return static_cast<SRT_ERRNO>(last_error().code);
    }
    static void close_accepts(ListenerRuntime& runtime)
    {
        runtime.accepts_.close();
    }
};
}

namespace {
using namespace robotweax::srt;
using namespace robotweax::srt::compat;
using namespace std::chrono_literals;

struct AdmissionFixture {
    std::atomic<unsigned> calls {0};
    std::atomic_bool reject_next {false};
    std::atomic_bool close_accepts_next {false};
    std::atomic<SRTSOCKET> last_admitted {SRT_INVALID_SOCK};
    std::vector<SRTSOCKET> sockets;
    SRTSOCKET listener = SRT_INVALID_SOCK;
    sockaddr_in address {};
    bool encrypted = false;
    static constexpr char passphrase[] = "local-admission-test-key";

    AdmissionFixture(
        bool groups = false, bool encryption = false, bool multicast = false)
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
        if (multicast) {
            sockaddr_in group_address {};
            group_address.sin_family = AF_INET;
            group_address.sin_port = htons(21000);
            group_address.sin_addr.s_addr = htonl(0xefff850eU);
            REQUIRE_EQ(srt_setsockflag(listener, SRTO_ROBOTWEAX_MCAST_GROUP,
                           &group_address, sizeof(group_address)),
                0);
        }
        configure_key(listener);
        REQUIRE_EQ(
            srt_listen_callback(
                listener,
                [](void* opaque, SRTSOCKET socket, int, const sockaddr*,
                    const char*) -> int {
                    auto& fixture = *static_cast<AdmissionFixture*>(opaque);
                    ++fixture.calls;
                    fixture.last_admitted = socket;
                    if (fixture.close_accepts_next.exchange(false))
                        ListenerRuntimeTestAccess::close_accepts(
                            *fixture.listener_record()->listener_runtime);
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
        bool ready = predicate();
        while (!ready && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
            ready = predicate();
        }
        REQUIRE(ready);
    }
    std::shared_ptr<SocketRecord> listener_record()
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
    REQUIRE_EQ(fixture.calls.load(), 1U);
    fixture.keep(srt_accept(fixture.listener, nullptr, nullptr));
    const auto next = fixture.keep(srt_create_group(SRT_GTYPE_BROADCAST));
    auto retry = srt_prepare_endpoint(nullptr,
        reinterpret_cast<sockaddr*>(&fixture.address), sizeof(fixture.address));
    REQUIRE(srt_connect_group(next, &retry, 1) != SRT_ERROR);
    fixture.keep(srt_accept(fixture.listener, nullptr, nullptr));
}

namespace {
struct FullListener {
    AdmissionFixture fixture;
    SRTSOCKET group = SRT_INVALID_SOCK;
    SRTSOCKET mirror = SRT_INVALID_SOCK;
    std::shared_ptr<SocketRecord> listener;
    std::shared_ptr<ListenerRuntime> runtime;
    std::shared_ptr<DatagramChannel> channel;
    std::uint32_t sequence = 0;
    bool dispatch = false;

    explicit FullListener(bool multicast = false)
        : fixture(!multicast, false, multicast)
        , dispatch(multicast)
    {
        group = fixture.keep(srt_create_group(
            multicast ? ROBOTWEAX_SRT_GTYPE_MULTICAST : SRT_GTYPE_BROADCAST));
        if (multicast) {
            REQUIRE_EQ(fixture.connect(fixture.caller()), 0);
        } else {
            auto endpoint = srt_prepare_endpoint(nullptr,
                reinterpret_cast<sockaddr*>(&fixture.address),
                sizeof(fixture.address));
            REQUIRE(srt_connect_group(group, &endpoint, 1) != SRT_ERROR);
        }
        listener = fixture.listener_record();
        REQUIRE(listener);
        fixture.wait([&] {
            return listener->pending_accepts.load() == 1U;
        });
        runtime = listener->listener_runtime;
        REQUIRE(runtime);
        fixture.wait([&] {
            return ListenerRuntimeTestAccess::pause_results(*runtime);
        });
        channel = listener->channel;
        REQUIRE(channel);
        // Synthetic post-cookie peers have no UDP receiver. Suppress their
        // replies so ICMP errors cannot affect the real listener on Windows.
        channel->set_send_hook_for_testing(
            [](std::span<const std::byte> bytes, IpEndpoint peer,
                void* opaque) noexcept -> UdpIoResult {
                if (peer.port >= 10000 && peer.port <= 17000)
                    return {.bytes_transferred = bytes.size()};
                return static_cast<DatagramChannel*>(opaque)->socket.send_to(
                    bytes, peer);
            },
            channel.get());
        const auto member =
            SocketRegistry::instance().find(fixture.last_admitted.load());
        REQUIRE(member);
        {
            std::lock_guard lock(member->mutex);
            mirror = member->group_id;
        }
        const auto peer_group = GroupRegistry::instance().find(group);
        REQUIRE(peer_group);
        {
            std::lock_guard lock(peer_group->mutex);
            sequence = peer_group->initial_sequence;
        }
    }
    ~FullListener()
    {
        if (channel)
            channel->set_send_hook_for_testing(nullptr, nullptr);
        if (runtime)
            ListenerRuntimeTestAccess::resume_results(*runtime);
    }
    ListenerHandshakeAdmission proposal(IpEndpoint peer, SRTSOCKET peer_group,
        SRT_GROUP_TYPE type = SRT_GTYPE_BROADCAST)
    {
        StatelessListenerHandshakeRouter::Configuration configuration;
        {
            std::lock_guard lock(listener->mutex);
            configuration.listener_socket_id = listener->protocol_socket_id;
            configuration.cookie_secret = listener->listener_cookie_secret;
        }
        ListenerHandshakeAdmissionSteps steps {configuration};
        HandshakeEnvelope envelope;
        envelope.peer = peer;
        envelope.message.packet.version = handshake_version_4;
        envelope.message.packet.extension_field = udt_datagram_socket_type;
        envelope.message.packet.request = HandshakeRequest::induction;
        envelope.message.packet.socket_id = 0x123456U;
        envelope.message.packet.initial_sequence = SequenceNumber {sequence};
        envelope.message.packet.maximum_transmission_unit = 1500;
        envelope.message.packet.flow_window = 8192;
        const auto induction = steps.receive(envelope, 0);
        REQUIRE_EQ(induction.kind, ListenerHandshakeAdmissionStepKind::send);
        envelope.control.destination_socket_id =
            configuration.listener_socket_id;
        envelope.message.packet.version = handshake_version_5;
        envelope.message.packet.request = HandshakeRequest::conclusion;
        envelope.message.packet.syn_cookie =
            induction.response.packet.syn_cookie;
        envelope.message.packet.extension_field = 1U;
        envelope.message.has_handshake_extension = true;
        envelope.message.extension_type =
            HandshakeExtensionType::handshake_request;
        envelope.message.extension_parameters =
            SocketOptions {}.handshake_parameters();
        envelope.message.has_group_membership = true;
        envelope.message.group_membership.group_id =
            static_cast<std::uint32_t>(peer_group);
        envelope.message.group_membership.type = dispatch
            ? GroupType::multicast
            : (type == SRT_GTYPE_BROADCAST ? GroupType::broadcast
                                           : GroupType::backup);
        const auto admitted = steps.receive(envelope, 0);
        REQUIRE_EQ(admitted.kind, ListenerHandshakeAdmissionStepKind::admit);
        return admitted.admission;
    }
    ListenerConnectionSetupActorResult take_result()
    {
        const auto actor = ListenerRuntimeTestAccess::actor(*runtime);
        REQUIRE(actor);
        fixture.wait([&] {
            return actor->snapshot().queued_results != 0U;
        });
        ListenerConnectionSetupActorResult result;
        REQUIRE(actor->try_pop(result));
        return result;
    }
    void rejected_attempt(IpEndpoint peer,
        std::chrono::steady_clock::time_point time, bool admitted)
    {
        const auto before = fixture.calls.load();
        const auto registered = SocketRegistry::instance().size();
        fixture.reject_next = true;
        const auto incoming = ListenerRuntimeTestAccess::admit(
            *runtime, listener, proposal(peer, group), time);
        if (admitted) {
            REQUIRE(incoming != SRT_INVALID_SOCK);
            auto result = take_result();
            REQUIRE_EQ(
                result.setup.outcome, ListenerConnectionSetupOutcome::rejected);
            (void)ListenerRuntimeTestAccess::complete(*runtime, result);
        } else {
            REQUIRE_EQ(incoming, SRT_INVALID_SOCK);
            REQUIRE_EQ(last_error().code, SRT_EASYNCRCV);
            REQUIRE(!ListenerRuntimeTestAccess::actor(*runtime));
        }
        REQUIRE_EQ(fixture.calls.load(), before + (admitted ? 1U : 0U));
        REQUIRE_EQ(SocketRegistry::instance().size(), registered);
        REQUIRE_EQ(listener->pending_accepts.load(), 1U);
    }
};
}

static void exercise_source_overflow_budget(bool multicast)
{
    FullListener test {multicast};
    const auto now = std::chrono::steady_clock::time_point {};
    for (unsigned index = 0; index < ListenerAdmissionBudget::source_burst;
        ++index)
        test.rejected_attempt(
            IpEndpoint::loopback(static_cast<std::uint16_t>(10000 + index)),
            now, true);
    test.rejected_attempt(IpEndpoint::loopback(11000), now, false);
    test.rejected_attempt(IpEndpoint::loopback(11001), now, false);
    test.rejected_attempt(IpEndpoint::loopback(11002), now + 1s, true);
}

static void exercise_global_overflow_budget(bool multicast)
{
    FullListener test {multicast};
    const auto now = std::chrono::steady_clock::time_point {};
    for (unsigned index = 0; index < ListenerAdmissionBudget::global_burst;
        ++index) {
        auto peer =
            IpEndpoint::loopback(static_cast<std::uint16_t>(12000 + index));
        peer.address[3] = static_cast<std::uint8_t>(1 + index / 16);
        test.rejected_attempt(peer, now, true);
    }
    auto peer = IpEndpoint::loopback(13000);
    peer.address[3] = 20;
    test.rejected_attempt(peer, now, false);
    test.rejected_attempt(peer, now + 1s, true);
}

TEST(listener_admission_unknown_bonding_group_drops_before_callback)
{
    FullListener test;
    const auto unknown =
        test.fixture.keep(srt_create_group(SRT_GTYPE_BROADCAST));
    const auto before = test.fixture.calls.load();
    const auto registered = SocketRegistry::instance().size();
    REQUIRE_EQ(ListenerRuntimeTestAccess::admit(*test.runtime, test.listener,
                   test.proposal(IpEndpoint::loopback(14000), unknown), {}),
        SRT_INVALID_SOCK);
    REQUIRE_EQ(test.fixture.calls.load(), before);
    REQUIRE_EQ(SocketRegistry::instance().size(), registered);
    REQUIRE(!ListenerRuntimeTestAccess::actor(*test.runtime));
}

TEST(listener_admission_existing_mirror_preflight_respects_listener_bonds)
{
    FullListener test;
    const auto other = test.fixture.keep(srt_create_socket());
    auto& registry = GroupRegistry::instance();
    REQUIRE(registry.has_existing_mirror(
        test.fixture.listener, test.group, SRT_GTYPE_BROADCAST));
    REQUIRE(
        !registry.has_existing_mirror(other, test.group, SRT_GTYPE_BROADCAST));
    REQUIRE(!registry.has_existing_mirror(
        test.fixture.listener, test.group, SRT_GTYPE_BACKUP));
    REQUIRE(!registry.has_existing_mirror(
        test.fixture.listener, test.mirror, SRT_GTYPE_BROADCAST));
    REQUIRE(registry.bond_listeners({test.fixture.listener, other}, 12345));
    REQUIRE(
        registry.has_existing_mirror(other, test.group, SRT_GTYPE_BROADCAST));
    registry.close(test.mirror);
    REQUIRE(
        !registry.has_existing_mirror(other, test.group, SRT_GTYPE_BROADCAST));
}

TEST(listener_admission_changed_group_result_cannot_attach_without_capacity)
{
    FullListener test;
    const auto incoming =
        ListenerRuntimeTestAccess::admit(*test.runtime, test.listener,
            test.proposal(IpEndpoint::loopback(15000), test.group), {});
    REQUIRE(incoming != SRT_INVALID_SOCK);
    auto result = test.take_result();
    REQUIRE_EQ(result.setup.outcome, ListenerConnectionSetupOutcome::connected);
    REQUIRE(result.runtime.has_peer_group_membership);
    // Inject a negotiated result that no longer matches its original group
    // claim. The production completion boundary must recheck accept capacity.
    result.runtime.has_peer_group_membership = false;
    REQUIRE_EQ(
        ListenerRuntimeTestAccess::complete(*test.runtime, result), SRT_ENOBUF);
    REQUIRE(!SocketRegistry::instance().find(incoming));
    REQUIRE_EQ(test.listener->pending_accepts.load(), 1U);
}

TEST(listener_admission_closed_accept_queue_reports_closed_during_completion)
{
    FullListener test;
    // Free the existing queued accept so this ordinary setup owns a slot.
    test.fixture.keep(srt_accept(test.fixture.listener, nullptr, nullptr));
    auto admission = test.proposal(IpEndpoint::loopback(16000), test.group);
    admission.conclusion.message.has_group_membership = false;
    const auto incoming = ListenerRuntimeTestAccess::admit(
        *test.runtime, test.listener, admission, {});
    REQUIRE(incoming != SRT_INVALID_SOCK);
    auto result = test.take_result();
    REQUIRE_EQ(result.setup.outcome, ListenerConnectionSetupOutcome::connected);
    ListenerRuntimeTestAccess::close_accepts(*test.runtime);
    REQUIRE_EQ(ListenerRuntimeTestAccess::complete(*test.runtime, result),
        SRT_ESCLOSED);
    REQUIRE(!SocketRegistry::instance().find(incoming));
}

TEST(listener_admission_bonding_overflow_exhausts_source_budget_before_callback)
{
    exercise_source_overflow_budget(false);
}
TEST(listener_admission_bonding_overflow_exhausts_global_budget_before_callback)
{
    exercise_global_overflow_budget(false);
}
TEST(
    listener_admission_multicast_overflow_exhausts_source_budget_before_callback)
{
    exercise_source_overflow_budget(true);
}
TEST(
    listener_admission_multicast_overflow_exhausts_global_budget_before_callback)
{
    exercise_global_overflow_budget(true);
}
TEST(listener_admission_closed_queue_reports_closed_for_first_group_member)
{
    FullListener test;
    test.fixture.keep(srt_accept(test.fixture.listener, nullptr, nullptr));
    const auto group = test.fixture.keep(srt_create_group(SRT_GTYPE_BROADCAST));
    test.fixture.close_accepts_next = true;
    const auto incoming = ListenerRuntimeTestAccess::admit(*test.runtime,
        test.listener, test.proposal(IpEndpoint::loopback(17000), group), {});
    REQUIRE(incoming != SRT_INVALID_SOCK);
    auto result = test.take_result();
    REQUIRE_EQ(result.setup.outcome, ListenerConnectionSetupOutcome::rejected);
    REQUIRE_EQ(ListenerRuntimeTestAccess::complete(*test.runtime, result),
        SRT_ESCLOSED);
    REQUIRE(!SocketRegistry::instance().find(incoming));
}
