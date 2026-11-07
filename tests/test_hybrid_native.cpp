// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/hybrid_native.hpp"
#include "compat/hybrid_receiver.hpp"
#include "compat/hybrid_automatic.hpp"
#include "compat/hybrid_automatic_return.hpp"
#include "compat/hybrid_membership.hpp"
#include <functional>
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/handshake_datagram.hpp"
#include <algorithm>
#include <atomic>
#include <future>
#include <vector>

using namespace robotweax::srt;
namespace h = robotweax::srt::compat::hybrid;
namespace {
h::ExchangeRecord offer()
{
    h::ExchangeRecord r {.kind = h::ExchangeKind::offer,
        .caller_socket = 107,
        .listener_socket = 108};
    r.capability = {};
    r.credential_id.fill(std::byte {0x11});
    r.authentication.version = 7;
    r.authentication.caller_nonce.fill(std::byte {1});
    return r;
}
std::array<std::byte, h::maximum_native_control_size> packet()
{
    std::array<std::byte, h::exchange_size> body {};
    REQUIRE(h::encode(offer(), body));
    std::array<std::byte, h::maximum_native_control_size> out {};
    REQUIRE(h::encode_exchange_packet(body, PacketTimestamp {1}, 108, out));
    return out;
}
}
TEST(hybrid_native_carrier_strict_size_direction_and_atomic_encoding)
{
    auto bytes = packet();
    const auto p = decode_packet(bytes);
    REQUIRE(p);
    REQUIRE(h::native_candidate(p.packet));
    h::ExchangeRecord decoded;
    REQUIRE(h::decode_exchange_packet(p.packet, decoded));
    REQUIRE_EQ(decoded.caller_socket, 107U);
    REQUIRE_EQ(decoded.listener_socket, 108U);
    auto view = p.packet;
    view.control.destination_socket_id = 107;
    REQUIRE(!h::decode_exchange_packet(view, decoded));
    view = p.packet;
    view.control.type_specific = 1;
    REQUIRE(!h::decode_exchange_packet(view, decoded));
    std::array<std::byte, h::exchange_size> body {};
    REQUIRE(h::encode(offer(), body));
    const auto before = bytes;
    REQUIRE(!h::encode_exchange_packet(body, PacketTimestamp {}, 107, bytes));
    REQUIRE_EQ(before, bytes);
    REQUIRE(!h::encode_exchange_packet(body, PacketTimestamp {}, 108,
        std::span {bytes}.first(bytes.size() - 1)));
    REQUIRE_EQ(before, bytes);
}
TEST(
    hybrid_native_candidate_separates_exchange_recovery_envelopes_and_ordinary_controls)
{
    auto bytes = packet();
    auto decoded = decode_packet(bytes);
    REQUIRE(decoded);
    auto p = decoded.packet;
    p.control.subtype = h::control_subtype;
    REQUIRE(h::native_candidate(p));
    p.control.subtype = authenticated_key_request_subtype;
    REQUIRE(!h::native_candidate(p));
    std::array<std::byte, 156> payload {};
    std::array<std::byte, packet_header_size + h::message_size> inner {};
    std::array<std::byte, h::message_size> body {};
    REQUIRE(encode_packet({.kind = PacketKind::control,
                              .control = {.type = ControlType::user_defined,
                                  .subtype = h::control_subtype},
                              .payload = body},
        inner));
    std::copy(inner.begin(), inner.end(), payload.begin() + 12);
    p.control.subtype = authenticated_control_subtype;
    p.payload = payload;
    REQUIRE(h::native_candidate(p));
    payload[15] ^= std::byte {1};
    REQUIRE(!h::native_candidate(p));
    p.kind = PacketKind::data;
    REQUIRE(!h::native_candidate(p));
}
TEST(hybrid_native_unconfigured_carrier_cannot_refresh_ordinary_peer_liveness)
{
    std::uint64_t now = 0;
    compat::ConnectionRuntime r({.peer = IpEndpoint::loopback(9000),
        .local_socket_id = 108,
        .peer_socket_id = 107,
        .peer_idle_timeout_milliseconds = 1,
        .now_function =
            [](void* p) noexcept {
                return *static_cast<std::uint64_t*>(p);
            },
        .now_context = &now});
    const auto bytes = packet();
    const auto decoded = decode_packet(bytes);
    REQUIRE(decoded);
    now = 999;
    r.process_packet(decoded.packet, IpEndpoint::loopback(9000));
    now = 1001;
    (void)r.poll();
    REQUIRE(r.broken());
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace robotweax::srt::compat {
struct HybridAutomaticTestAccess {
    static std::shared_ptr<DatagramChannel> membership_channel(
        hybrid::MembershipRecovery& m)
    {
        std::lock_guard lock(m.mutex_);
        return m.channel_;
    }
    static std::shared_ptr<const MulticastIngressBinding> token(
        ConnectionRuntime& r)
    {
        std::lock_guard lock(r.mutex_);
        return r.multicast_ingress_binding_;
    }
    static void link(const std::shared_ptr<ConnectionRuntime>& r,
        const std::shared_ptr<hybrid::NativeService>& service)
    {
        std::lock_guard lock(r->mutex_);
        r->hybrid_service_ = service;
        service->runtime_ = r;
    }
    static void channel(const std::shared_ptr<ConnectionRuntime>& r,
        const std::shared_ptr<DatagramChannel>& c)
    {
        std::lock_guard lock(r->mutex_);
        r->channel_ = c;
    }
    static void multicast(const std::shared_ptr<ConnectionRuntime>& r,
        const std::shared_ptr<hybrid::NativeService>& service,
        std::uint32_t sequence, std::uint64_t now, bool multicast = true,
        std::shared_ptr<const MulticastIngressBinding> stale = {})
    {
        std::shared_ptr<const MulticastIngressBinding> binding;
        {
            std::lock_guard lock(r->mutex_);
            binding = r->multicast_ingress_binding_;
        }
        if (stale)
            binding = std::move(stale);
        const std::array<std::byte, 1> payload {std::byte {1}};
        r->process_packet({.kind = PacketKind::data,
                              .data = {.sequence = SequenceNumber {sequence},
                                  .message_number = sequence - 41,
                                  .boundary = MessageBoundary::solo,
                                  .destination_socket_id =
                                      service->origin()->dispatch().group_id},
                              .payload = payload},
            multicast ? service->origin()->source().endpoint
                      : IpEndpoint::loopback(9000),
            ConnectionRuntime::Clock::time_point {
                std::chrono::microseconds {now}},
            multicast ? PacketIngress::multicast_data
                      : PacketIngress::unicast_session,
            multicast ? binding : nullptr);
    }
};
}
namespace {
std::shared_ptr<const h::Conclusions> conclusions()
{
    h::ConclusionCapture capture;
    const MulticastDispatch dispatch {.group_id = group_handle_mask | 1U,
        .multicast_group = {.address = {239, 255, 1, 1}, .port = 6000},
        .initial_sequence = 42,
        .time_to_live = 1};
    const MulticastSource source {.kind = MulticastSourceKind::announcement,
        .group_id = dispatch.group_id,
        .endpoint = {.address = {192, 0, 2, 1}, .port = 9000}};
    for (bool caller : {true, false}) {
        HandshakeAction a;
        a.kind = HandshakeActionKind::send;
        a.packet.request = HandshakeRequest::conclusion;
        a.packet.socket_id = caller ? 107 : 108;
        a.packet.initial_sequence = SequenceNumber {1};
        a.packet.extension_field = handshake_extension_flag_handshake
            | handshake_extension_flag_config;
        a.has_handshake_extension = true;
        a.extension_type = caller ? HandshakeExtensionType::handshake_request
                                  : HandshakeExtensionType::handshake_response;
        a.has_group_membership = true;
        a.group_membership = {
            .group_id = caller ? (group_handle_mask | 2U) : dispatch.group_id,
            .type = GroupType::multicast};
        a.has_multicast_source = true;
        if (caller)
            a.multicast_source.kind = MulticastSourceKind::offer;
        else {
            a.has_multicast_dispatch = true;
            a.multicast_dispatch = dispatch;
            a.multicast_source = source;
        }
        std::array<std::byte, 1500> wire {};
        const auto e =
            encode_handshake_datagram(a, PacketTimestamp {}, 0, wire);
        REQUIRE(e);
        REQUIRE(capture.observe(std::span {wire}.first(e.bytes_written)));
    }
    auto pair = capture.freeze(true, 107, 108);
    REQUIRE(pair);
    return pair;
}
struct Clock {
    std::atomic<std::uint64_t> value = 100;
    static std::uint64_t read(void* p) noexcept
    {
        return static_cast<Clock*>(p)->value.load();
    }
};
struct Provider {
    std::atomic<unsigned> resolves = 0, rechecks = 0, enrolls = 0;
    std::atomic<std::uint64_t> generation = 7, expiry = 10000;
    bool allow = true, enroll = true, close_resolve = false,
         close_enroll = false;
    Clock* expire_during_revalidation = nullptr;
    std::weak_ptr<h::NativeService> service;
    static bool resolve(void* p, const h::CredentialRequest& r,
        h::Authorization& a, std::span<std::byte, 32> secret) noexcept
    {
        auto& self = *static_cast<Provider*>(p);
        ++self.resolves;
        a = {.credential_id = r.credential_id,
            .caller_group = r.caller_group,
            .listener_group = r.listener_group,
            .stream_id = r.stream_id,
            .generation = self.generation.load(),
            .expires_at_us = self.expiry.load()};
        std::fill(secret.begin(), secret.end(), std::byte {0x22});
        if (self.close_resolve)
            if (auto s = self.service.lock())
                s->close();
        return self.allow;
    }
    static bool revalidate(void* p, const h::CredentialRequest&,
        const h::Authorization& expected, h::Authorization& a) noexcept
    {
        auto& self = *static_cast<Provider*>(p);
        ++self.rechecks;
        a = expected;
        if (self.expire_during_revalidation)
            self.expire_during_revalidation->value = self.expiry.load();
        a.generation = self.generation.load();
        a.expires_at_us = self.expiry.load();
        return self.allow;
    }
    static bool activate(void* p, const h::Context&, const h::Authorization&,
        std::uint64_t) noexcept
    {
        auto& self = *static_cast<Provider*>(p);
        ++self.enrolls;
        if (self.close_enroll)
            if (auto s = self.service.lock())
                s->close();
        return self.enroll;
    }
};
struct Transport {
    std::function<void()> bound_effect;
    bool alive = true, partial = false, close_send = false;
    unsigned would_block = 0, drop_kind = 0, dropped = 0;
    std::weak_ptr<h::NativeService> service;
    std::mutex mutex;
    std::vector<std::vector<std::byte>> output, history;
    static bool bound(void* p, const h::NativeService*) noexcept
    {
        auto& self = *static_cast<Transport*>(p);
        if (self.bound_effect)
            self.bound_effect();
        return self.alive;
    }
    static UdpIoResult send(void* p, const h::NativeService*,
        std::span<const std::byte> bytes, std::uint64_t) noexcept
    {
        auto& self = *static_cast<Transport*>(p);
        if (self.close_send)
            if (auto s = self.service.lock())
                s->close();
        if (self.partial)
            return {.bytes_transferred = 1};
        if (self.would_block) {
            --self.would_block;
            return {.error = Error::would_block};
        }
        std::lock_guard lock(self.mutex);
        self.history.emplace_back(bytes.begin(), bytes.end());
        const auto packet = decode_packet(bytes);
        h::ExchangeRecord r;
        if (packet && h::decode_exchange_packet(packet.packet, r)
            && static_cast<unsigned>(r.kind) == self.drop_kind && !self.dropped)
            ++self.dropped;
        else
            self.output.emplace_back(bytes.begin(), bytes.end());
        return {.bytes_transferred = bytes.size()};
    }
    std::vector<std::vector<std::byte>> take()
    {
        std::lock_guard lock(mutex);
        auto v = std::move(output);
        output = {};
        return v;
    }
};
struct Pair {
    bool returning = false, startup = false;
    std::shared_ptr<Clock> clock = std::make_shared<Clock>();
    std::shared_ptr<Provider> cp = std::make_shared<Provider>(),
                              lp = std::make_shared<Provider>();
    std::shared_ptr<Transport> ct = std::make_shared<Transport>(),
                               lt = std::make_shared<Transport>();
    std::shared_ptr<const h::Conclusions> origin = conclusions();
    std::shared_ptr<h::NativeService> c, l;
    h::NativeService::Configuration config(bool caller)
    {
        h::NativeService::Configuration x;
        x.caller = caller;
        x.local_socket = caller ? 107 : 108;
        x.peer_socket = caller ? 108 : 107;
        x.conclusions = origin;
        x.exchange.dispatch = origin->dispatch();
        x.exchange.source = origin->source();
        x.exchange.credential_id.fill(std::byte {0x11});
        x.exchange.established_peer =
            IpEndpoint::loopback(caller ? 8000 : 8001);
        x.exchange.connection_generation = caller ? 1 : 2;
        x.exchange.retry_interval_us = 100;
        x.exchange.timeout_us = 1000;
        x.exchange.policy.supported =
            returning ? h::return_features : h::policy_query_features;
        if (startup)
            x.exchange.policy.supported |= h::startup_fallback;
        if (!caller) {
            x.exchange.stream = 71;
            x.exchange.member = 97;
            x.exchange.initial_position = 1234;
        }
        x.callbacks.owner = caller ? cp : lp;
        x.callbacks.resolve = Provider::resolve;
        x.callbacks.revalidate = Provider::revalidate;
        x.callbacks.enroll = Provider::activate;
        x.transport_owner = caller ? ct : lt;
        x.bound = Transport::bound;
        x.transmit = Transport::send;
        x.clock = Clock::read;
        x.clock_context = clock.get();
        return x;
    }
    explicit Pair(bool returns = false, bool starts = false)
        : returning(returns)
        , startup(starts)
    {
        c = h::NativeService::create_for_testing(config(true));
        l = h::NativeService::create_for_testing(config(false));
        REQUIRE(c);
        REQUIRE(l);
        cp->service = c;
        lp->service = l;
        ct->service = c;
        lt->service = l;
    }
    void deliver(const std::shared_ptr<Transport>& t,
        const std::shared_ptr<h::NativeService>& s)
    {
        for (auto& wire : t->take()) {
            const auto p = decode_packet(wire);
            REQUIRE(p);
            (void)s->enqueue(p.packet, clock->value.load());
        }
    }
    void round()
    {
        c->pump_for_testing();
        deliver(ct, l);
        l->pump_for_testing();
        deliver(lt, c);
    }
    void connect()
    {
        REQUIRE(l->activate());
        REQUIRE(c->activate());
        for (unsigned i = 0; i < 6; ++i) {
            round();
            ++clock->value;
        }
        REQUIRE(c->snapshot().active);
        REQUIRE(l->snapshot().active);
    }
    static h::Message request(std::uint64_t serial = 1)
    {
        return {.kind = h::Kind::fallback_request,
            .stream = 71,
            .member = 97,
            .serial = serial,
            .next_position = 1234,
            .next_sequence = 42,
            .mode = h::Mode::unicast};
    }
    static h::Message report(std::uint64_t serial = 1)
    {
        return {.kind = h::Kind::path_report,
            .stream = 71,
            .member = 97,
            .serial = serial,
            .next_position = 1234,
            .next_sequence = 42,
            .multicast_next = 1234,
            .interval_us = 100};
    }
};
}
TEST(
    hybrid_native_owner_exchanges_proofs_enrolls_and_delivers_authenticated_commands)
{
    Pair p;
    p.connect();
    REQUIRE_EQ(p.cp->resolves.load(), 1U);
    REQUIRE_EQ(p.lp->resolves.load(), 1U);
    REQUIRE_EQ(p.cp->enrolls.load(), 1U);
    REQUIRE_EQ(p.lp->enrolls.load(), 1U);
    REQUIRE_EQ(p.c->send(Pair::request(), 1000),
        h::NativeService::Admission::accepted);
    p.round();
    h::NativeService::Event e;
    REQUIRE(p.c->pop(e));
    REQUIRE_EQ(e.kind, h::NativeService::Event::Kind::submitted);
    REQUIRE(p.l->pop(e));
    REQUIRE_EQ(e.received.status, h::Session::Status::accepted);
    REQUIRE_EQ(e.message, Pair::request());
    REQUIRE_EQ(e.generation, 2U);
    const auto at = e.received.first_received_us;
    ++p.clock->value;
    REQUIRE_EQ(p.c->send(Pair::request(), 1000),
        h::NativeService::Admission::accepted);
    p.round();
    REQUIRE(p.l->pop(e));
    REQUIRE_EQ(e.received.status, h::Session::Status::duplicate);
    REQUIRE_EQ(e.received.first_received_us, at);
}
TEST(
    hybrid_native_loss_of_each_carrier_recovers_without_new_credential_resolution)
{
    for (unsigned kind = 1; kind <= 4; ++kind) {
        Pair p;
        (kind == 1 || kind == 3 ? p.ct : p.lt)->drop_kind = kind;
        REQUIRE(p.l->activate());
        REQUIRE(p.c->activate());
        for (unsigned i = 0; i < 9; ++i) {
            p.round();
            p.clock->value += 100;
            if (p.c->snapshot().active && p.l->snapshot().active)
                break;
        }
        REQUIRE(p.c->snapshot().active);
        REQUIRE(p.l->snapshot().active);
        REQUIRE_EQ(p.cp->resolves.load(), 1U);
        REQUIRE_EQ(p.lp->resolves.load(), 1U);
    }
}
TEST(
    hybrid_native_callbacks_can_close_without_deadlock_or_publishing_activation)
{
    for (unsigned mode = 0; mode < 4; ++mode) {
        Pair p;
        p.lp->close_resolve = mode == 0;
        p.lp->close_enroll = mode == 1;
        p.lp->enroll = mode != 2;
        if (mode == 3)
            p.lp->expire_during_revalidation = p.clock.get();
        REQUIRE(p.l->activate());
        REQUIRE(p.c->activate());
        for (unsigned i = 0; i < 5; ++i)
            p.round();
        REQUIRE(p.l->snapshot().closed);
        REQUIRE(!p.l->snapshot().active);
        REQUIRE(!p.c->snapshot().active);
        h::NativeService::Event e;
        REQUIRE(!p.l->pop(e));
    }
}
TEST(hybrid_native_expiry_fences_queues_even_before_the_timer_runs)
{
    Pair p;
    p.connect();
    REQUIRE_EQ(p.c->send(Pair::request(), 1000),
        h::NativeService::Admission::accepted);
    p.clock->value = 10000;
    h::NativeService::Event e;
    REQUIRE(!p.c->pop(e));
    REQUIRE(p.c->snapshot().closed);
    p.c->pump_for_testing();
    REQUIRE(p.ct->take().empty());
    REQUIRE_EQ(
        p.c->send(Pair::request(), 10100), h::NativeService::Admission::closed);
}
TEST(
    hybrid_native_revocation_and_connection_binding_changes_cancel_pending_work)
{
    for (bool revocation : {false, true}) {
        Pair p;
        p.connect();
        REQUIRE_EQ(p.c->send(Pair::request(), 1000),
            h::NativeService::Admission::accepted);
        if (revocation)
            ++p.cp->generation;
        else
            p.ct->alive = false;
        p.c->pump_for_testing();
        REQUIRE(p.c->snapshot().closed);
        REQUIRE(p.ct->take().empty());
    }
}
TEST(
    hybrid_native_report_age_includes_local_queue_wait_and_retries_require_new_serial)
{
    Pair p;
    p.connect();
    const auto observed = p.clock->value.load();
    REQUIRE_EQ(p.c->send(Pair::report(), 1000, observed),
        h::NativeService::Admission::accepted);
    p.clock->value += 200;
    p.round();
    h::NativeService::Event e;
    REQUIRE(p.l->pop(e));
    REQUIRE_EQ(e.message.age_us, 200U);
    REQUIRE_EQ(e.received.first_received_us, p.clock->value.load());
    REQUIRE(p.c->pop(e));
    REQUIRE_EQ(p.c->send(Pair::report(), 1000, observed),
        h::NativeService::Admission::accepted);
    p.round();
    REQUIRE(p.c->pop(e));
    REQUIRE_EQ(e.kind, h::NativeService::Event::Kind::rejected);
    REQUIRE(!p.l->pop(e));
    REQUIRE_EQ(p.c->send(Pair::report(2), 1000, p.clock->value.load()),
        h::NativeService::Admission::accepted);
    p.round();
    REQUIRE(p.l->pop(e));
    REQUIRE_EQ(e.message.serial, 2U);
}
TEST(hybrid_native_expired_command_does_not_consume_a_semantic_serial)
{
    Pair p;
    p.connect();
    REQUIRE_EQ(
        p.c->send(Pair::request(), 200), h::NativeService::Admission::accepted);
    p.clock->value = 200;
    p.round();
    h::NativeService::Event e;
    REQUIRE(p.c->pop(e));
    REQUIRE_EQ(e.kind, h::NativeService::Event::Kind::expired);
    REQUIRE(!p.l->pop(e));
    REQUIRE_EQ(p.c->send(Pair::request(), 1000),
        h::NativeService::Admission::accepted);
    p.round();
    REQUIRE(p.l->pop(e));
    REQUIRE_EQ(e.received.status, h::Session::Status::accepted);
}
TEST(
    hybrid_native_multicast_ready_observation_age_is_not_freshened_by_queue_wait)
{
    Pair p;
    p.connect();
    auto ready = Pair::report();
    ready.kind = h::Kind::multicast_ready;
    REQUIRE_EQ(p.c->send(ready, 1000), h::NativeService::Admission::invalid);
    const auto observed = p.clock->value.load();
    REQUIRE_EQ(p.c->send(ready, 1000, observed),
        h::NativeService::Admission::accepted);
    p.clock->value += 200;
    p.round();
    h::NativeService::Event e;
    REQUIRE(p.l->pop(e));
    REQUIRE_EQ(e.message.age_us, 200U);
}
TEST(
    hybrid_native_udp_backpressure_is_visible_and_command_retry_uses_fresh_envelope)
{
    Pair p;
    p.connect();
    p.ct->would_block = 1;
    REQUIRE_EQ(p.c->send(Pair::request(), 1000),
        h::NativeService::Admission::accepted);
    p.round();
    h::NativeService::Event e;
    REQUIRE(p.c->pop(e));
    REQUIRE_EQ(e.kind, h::NativeService::Event::Kind::retryable);
    REQUIRE(!p.l->pop(e));
    REQUIRE_EQ(p.c->send(Pair::request(), 1000),
        h::NativeService::Admission::accepted);
    p.round();
    REQUIRE(p.l->pop(e));
    REQUIRE_EQ(e.received.status, h::Session::Status::accepted);
    REQUIRE_EQ(p.c->snapshot().retryable, 1U);
}
TEST(
    hybrid_native_bounded_queues_refuse_overflow_and_event_backpressure_fails_closed)
{
    Pair p;
    p.connect();
    for (unsigned i = 0; i < h::NativeService::capacity; ++i)
        REQUIRE_EQ(p.c->send(Pair::request(i + 1), 1000),
            h::NativeService::Admission::accepted);
    REQUIRE_EQ(
        p.c->send(Pair::request(99), 1000), h::NativeService::Admission::full);
    p.c->pump_for_testing();
    REQUIRE_EQ(p.c->snapshot().events, h::NativeService::event_capacity);
    p.c->pump_for_testing();
    REQUIRE(p.c->snapshot().closed);
    REQUIRE_EQ(p.c->snapshot().failure, h::NativeService::Failure::events_full);
}
TEST(
    hybrid_native_partial_udp_or_reentrant_send_close_cannot_publish_success_events)
{
    for (bool partial : {false, true}) {
        Pair p;
        p.connect();
        p.ct->partial = partial;
        p.ct->close_send = !partial;
        REQUIRE_EQ(p.c->send(Pair::request(), 1000),
            h::NativeService::Admission::accepted);
        p.c->pump_for_testing();
        REQUIRE(p.c->snapshot().closed);
        h::NativeService::Event e;
        REQUIRE(!p.c->pop(e));
    }
}
TEST(
    hybrid_native_forged_control_is_dropped_without_events_or_crypto_replay_poisoning)
{
    Pair p;
    p.connect();
    REQUIRE_EQ(p.c->send(Pair::request(), 1000),
        h::NativeService::Admission::accepted);
    p.c->pump_for_testing();
    auto frames = p.ct->take();
    REQUIRE_EQ(frames.size(), 1U);
    const auto original = frames.front();
    for (std::size_t i = 0; i < original.size(); ++i) {
        auto changed = original;
        changed[i] ^= std::byte {1};
        const auto decoded = decode_packet(changed);
        if (decoded)
            (void)p.l->enqueue(decoded.packet, p.clock->value.load());
        p.l->pump_for_testing();
        h::NativeService::Event e;
        REQUIRE(!p.l->pop(e));
        REQUIRE(p.l->snapshot().active);
    }
    const auto decoded = decode_packet(original);
    REQUIRE(decoded);
    REQUIRE(p.l->enqueue(decoded.packet, p.clock->value.load()));
    p.l->pump_for_testing();
    h::NativeService::Event e;
    REQUIRE(p.l->pop(e));
    REQUIRE_EQ(e.received.status, h::Session::Status::accepted);
}
TEST(
    hybrid_native_scheduler_service_capacity_and_callback_owner_lifetime_are_bounded)
{
    Pair p;
    auto scheduler = std::make_shared<compat::RuntimeScheduler>(
        compat::RuntimeScheduler::Configuration {.shard_count = 1,
            .queue_capacity_per_shard = 1,
            .timer_capacity_per_shard = 1,
            .service_capacity_per_shard = 1});
    REQUIRE(scheduler->start());
    auto cfg = p.config(true);
    cfg.clock = nullptr;
    cfg.clock_context = nullptr;
    auto first = h::NativeService::create(cfg, scheduler);
    REQUIRE(first);
    REQUIRE(!h::NativeService::create(cfg, scheduler));
    first->close();
    REQUIRE_EQ(scheduler->snapshot().services_reserved, 0U);
    auto second = h::NativeService::create(cfg, scheduler);
    REQUIRE(second);
    second->close();
    scheduler->stop();
}
namespace {
struct ReceiverFixture {
    Pair p;
    std::shared_ptr<h::ReceiverBridge> receiver;
    std::vector<h::Message> requests;
    std::shared_ptr<compat::ConnectionRuntime> runtime;
    std::shared_ptr<compat::DatagramChannel> multicast;
    std::uint64_t reply_serial = 1;
    static h::ReceiverBridge::Configuration configuration()
    {
        return {.retry_interval_us = 10,
            .round_timeout_us = 100,
            .operation_timeout_us = 400,
            .query_interval_us = 200,
            .confirmation_lifetime_us = 150,
            .maximum_sends = 2,
            .maximum_rounds = 3};
    }
    explicit ReceiverFixture(
        h::ReceiverBridge::Configuration c = configuration(),
        bool local_data = false, bool returns = false, bool starts = false)
        : p(returns, starts)
    {
        p.connect();
        if (local_data) {
            const auto pair = p.c->origin();
            runtime = std::make_shared<compat::ConnectionRuntime>(
                compat::ConnectionRuntime::Configuration {
                    .peer = IpEndpoint::loopback(9000),
                    .local_socket_id = 107,
                    .peer_socket_id = 108,
                    .initial_sequence = SequenceNumber {42},
                    .peer_wire_initial_sequence = SequenceNumber {42}});
            multicast = std::make_shared<compat::DatagramChannel>();
            REQUIRE(multicast->register_ingress_route(pair->dispatch().group_id,
                runtime, compat::PacketIngress::multicast_data,
                compat::MulticastIngressConfiguration {
                    .source = pair->source()}));
            REQUIRE(runtime->publish_hybrid_conclusions(pair, true));
            compat::HybridAutomaticTestAccess::link(runtime, p.c);
        }
        receiver = h::ReceiverBridge::create(p.c, c);
        REQUIRE(receiver);
    }
    void step()
    {
        receiver->pump();
        p.round();
        p.round();
        receiver->pump();
        h::NativeService::Event e;
        while (p.l->pop(e))
            if (e.kind == h::NativeService::Event::Kind::received)
                requests.push_back(e.message);
    }
    h::Message response(h::Kind kind = h::Kind::delivery_status,
        std::uint64_t revision = 0, h::Mode mode = h::Mode::multicast,
        std::uint64_t position = 1234, std::uint64_t reference = UINT64_MAX)
    {
        return {.kind = kind,
            .stream = 71,
            .member = 97,
            .serial = reply_serial++,
            .revision = revision,
            .next_position = position,
            .next_sequence = static_cast<std::uint32_t>(
                (42 + (position - 1234)) & SequenceNumber::mask),
            .mode = mode,
            .retained_floor = 1234,
            .multicast_next = (p.c->snapshot().context.selection.supported
                                  & h::return_control)
                ? position
                : 0,
            .request_serial = reference == UINT64_MAX
                ? receiver->snapshot().request_serial
                : reference};
    }
    void deliver(h::Message m)
    {
        REQUIRE_EQ(p.l->send(m, p.clock->value.load() + 1000),
            h::NativeService::Admission::accepted);
        step();
        step();
    }
    void advance(std::uint64_t n)
    {
        p.clock->value += n;
        step();
    }
};
}
namespace {
struct AutomaticProvider {
    std::shared_ptr<Clock> clock;
    std::function<void(const h::AutomaticFallback::Request&)> effect;
    unsigned calls = 0;
    bool available = true, failed = false;
    std::uint64_t position = 1234, age = 0, generation = 0;
    static std::optional<h::AutomaticFallback::Cursor> resolve(
        void* p, const h::AutomaticFallback::Request& request) noexcept
    {
        auto& self = *static_cast<AutomaticProvider*>(p);
        ++self.calls;
        try {
            if (self.effect)
                self.effect(request);
        } catch (...) {
            self.failed = true;
            return {};
        }
        if (!self.available)
            return {};
        return h::AutomaticFallback::Cursor {self.position,
            self.clock->value.load() - self.age,
            self.generation ? self.generation : request.generation};
    }
};
struct AutomaticFixture : ReceiverFixture {
    std::shared_ptr<AutomaticProvider> provider =
        std::make_shared<AutomaticProvider>();
    std::shared_ptr<h::AutomaticFallback> controller;
    static h::ReceiverBridge::Configuration configuration()
    {
        auto c = ReceiverFixture::configuration();
        c.detection = h::DetectionConfiguration {.report_age_us = 1000,
            .maximum_transit_us = 10,
            .stall_us = 50,
            .emission_lifetime_us = 300,
            .recovery_us = 40,
            .recovery_gap_us = 30,
            .hold_down_us = 80,
            .sample_gap_us = 20,
            .recovery_advances = 3};
        return c;
    }
    explicit AutomaticFixture(
        std::uint32_t operations = 3, std::uint32_t cursor_lifetime = 1000)
        : ReceiverFixture(configuration(), true)
    {
        provider->clock = p.clock;
        controller = h::AutomaticFallback::create(receiver,
            {operations, cursor_lifetime},
            {provider, AutomaticProvider::resolve});
        REQUIRE(controller);
    }
    void progress(std::uint64_t offset = 0,
        h::Activity activity = h::Activity::producing,
        std::uint64_t revision = 0)
    {
        auto m = response(h::Kind::delivery_status, revision,
            h::Mode::multicast, 1234 + offset);
        m.kind = h::Kind::stream_progress;
        m.request_serial = 0;
        m.multicast_next = m.next_position;
        m.produced_next = 1334;
        m.interval_us = 10;
        m.activity = activity;
        REQUIRE_EQ(
            p.l->send(m, p.clock->value.load() + 1000, p.clock->value.load()),
            h::NativeService::Admission::accepted);
        step();
        step();
    }
    void candidate(std::uint64_t revision = 0, std::uint64_t offset = 0)
    {
        step();
        deliver(response(h::Kind::delivery_status, revision, h::Mode::multicast,
            1234 + offset));
        progress(offset, h::Activity::producing, revision);
        advance(10);
        progress(offset + 1, h::Activity::producing, revision);
        for (unsigned i = 0; i < 6; ++i)
            advance(10);
        const auto a = receiver->path_assessment();
        REQUIRE(a);
        if (!a->fallback_candidate)
            throw std::runtime_error("automatic candidate state="
                + std::to_string(static_cast<unsigned>(a->state)) + " reason="
                + std::to_string(static_cast<unsigned>(a->reason)) + " data="
                + std::to_string(receiver->data_progress().has_value())
                + " policy=" + std::to_string(receiver->snapshot().fresh));
    }
};
}
TEST(
    hybrid_automatic_factory_requires_detector_provider_bounds_and_exclusive_single_use_attachment)
{
    ReceiverFixture plain;
    auto provider = std::make_shared<AutomaticProvider>();
    provider->clock = plain.p.clock;
    REQUIRE(!h::AutomaticFallback::create(
        plain.receiver, {1, 1000}, {provider, AutomaticProvider::resolve}));
    AutomaticFixture f;
    REQUIRE(!h::AutomaticFallback::create(
        f.receiver, {1, 1000}, {f.provider, AutomaticProvider::resolve}));
    f.controller->close();
    f.controller.reset();
    REQUIRE(!h::AutomaticFallback::create(
        f.receiver, {1, 1000}, {f.provider, AutomaticProvider::resolve}));
    REQUIRE(!h::AutomaticFallback::create(
        f.receiver, {0, 1000}, {f.provider, AutomaticProvider::resolve}));
    REQUIRE(!h::AutomaticFallback::create(
        f.receiver, {9, 1000}, {f.provider, AutomaticProvider::resolve}));
    REQUIRE(!h::AutomaticFallback::create(
        f.receiver, {1, 0}, {f.provider, AutomaticProvider::resolve}));
    REQUIRE(!h::AutomaticFallback::create(
        f.receiver, {1, 1000}, {f.provider, nullptr}));
}
TEST(
    hybrid_automatic_fresh_candidate_posts_one_owned_operation_and_preserves_cursor)
{
    AutomaticFixture f;
    f.candidate();
    f.controller->pump();
    REQUIRE_EQ(f.provider->calls, 1U);
    auto state = f.controller->snapshot();
    REQUIRE_EQ(state.admitted, 1U);
    REQUIRE_EQ(state.position, 1234U);
    f.step();
    REQUIRE_EQ(f.requests.back().kind, h::Kind::fallback_request);
    REQUIRE_EQ(f.requests.back().next_position, 1234U);
    f.deliver(f.response(h::Kind::delivery_commit, 1, h::Mode::unicast, 1235));
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::confirmed);
    for (unsigned i = 0; i < 20; ++i)
        f.controller->pump();
    REQUIRE_EQ(f.provider->calls, 1U);
    REQUIRE(!f.provider->failed);
}
TEST(
    hybrid_automatic_unavailable_provider_and_repeated_candidates_cannot_refresh_attempt_budget)
{
    AutomaticFixture f;
    f.provider->available = false;
    f.candidate();
    for (unsigned i = 0; i < 30; ++i)
        f.controller->pump();
    REQUIRE_EQ(f.provider->calls, 1U);
    REQUIRE_EQ(f.controller->snapshot().phase,
        h::AutomaticFallback::Phase::unavailable);
    REQUIRE_EQ(
        f.receiver->snapshot().fallback, h::ReceiverBridge::RequestState::none);
}
TEST(
    hybrid_automatic_cursor_scope_age_future_position_and_provider_delay_are_revalidated)
{
    for (unsigned mode = 0; mode < 6; ++mode) {
        AutomaticFixture f(3, 100);
        f.candidate();
        if (mode == 0)
            f.provider->generation = 99;
        if (mode == 1)
            f.provider->age = 100;
        if (mode == 2)
            f.provider->position = 1235;
        if (mode == 3)
            f.provider->position = 1233;
        if (mode == 4)
            f.provider->effect = [&](const auto&) {
                f.p.clock->value += 21;
            };
        if (mode == 5)
            f.provider->age = f.p.clock->value.load() + 1;
        f.controller->pump();
        REQUIRE_EQ(f.controller->snapshot().phase,
            h::AutomaticFallback::Phase::refused);
        REQUIRE_EQ(f.controller->snapshot().admitted, 0U);
        REQUIRE_EQ(f.receiver->snapshot().fallback,
            h::ReceiverBridge::RequestState::none);
    }
}
TEST(
    hybrid_automatic_callbacks_can_inspect_reenter_close_or_lose_the_route_without_deadlock)
{
    for (unsigned mode = 0; mode < 4; ++mode) {
        AutomaticFixture f;
        f.candidate();
        f.provider->effect = [&](const auto&) {
            (void)f.receiver->snapshot();
            (void)f.controller->snapshot();
            f.controller->pump();
            if (mode == 0)
                f.controller->close();
            if (mode == 1)
                f.receiver->close();
            if (mode == 2)
                f.multicast->unregister_connection(
                    f.p.c->origin()->dispatch().group_id);
            if (mode == 3)
                f.progress(1, h::Activity::paused);
        };
        f.controller->pump();
        REQUIRE_EQ(f.provider->calls, 1U);
        REQUIRE(!f.provider->failed);
        REQUIRE_EQ(f.controller->snapshot().admitted, 0U);
    }
}
TEST(
    hybrid_automatic_manual_intent_wins_even_if_it_finishes_during_cursor_callback)
{
    AutomaticFixture f;
    f.candidate();
    f.provider->effect = [&](const auto&) {
        REQUIRE_EQ(f.receiver->request_fallback(1234),
            h::ReceiverBridge::Admission::accepted);
        f.step();
        auto denied = f.response();
        denied.outcome = h::Outcome::budget;
        f.deliver(denied);
    };
    f.controller->pump();
    REQUIRE(!f.provider->failed);
    REQUIRE_EQ(f.controller->snapshot().admitted, 0U);
    REQUIRE_EQ(
        f.controller->snapshot().phase, h::AutomaticFallback::Phase::refused);
}
TEST(
    hybrid_automatic_denial_is_attributed_and_cannot_restart_under_same_revision)
{
    AutomaticFixture f;
    f.candidate();
    f.controller->pump();
    f.step();
    auto denied = f.response();
    denied.outcome = h::Outcome::budget;
    f.deliver(denied);
    auto result = f.controller->snapshot();
    REQUIRE_EQ(result.result, h::ReceiverBridge::RequestState::denied);
    REQUIRE_EQ(result.outcome, h::Outcome::budget);
    for (unsigned i = 0; i < 20; ++i)
        f.controller->pump();
    REQUIRE_EQ(f.provider->calls, 1U);
    REQUIRE_EQ(f.receiver->request_fallback(1234),
        h::ReceiverBridge::Admission::accepted);
    f.step();
    f.deliver(f.response(h::Kind::delivery_commit, 1, h::Mode::unicast, 1235));
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::denied);
}
TEST(
    hybrid_automatic_close_prevents_new_admissions_but_preserves_receiver_owned_handoff)
{
    AutomaticFixture f;
    f.candidate();
    f.controller->pump();
    f.controller->close();
    REQUIRE(!f.receiver->snapshot().closed);
    f.step();
    REQUIRE_EQ(f.requests.back().kind, h::Kind::fallback_request);
    f.deliver(f.response(h::Kind::delivery_commit, 1, h::Mode::unicast, 1235));
    auto result = f.controller->snapshot();
    REQUIRE(result.closed);
    REQUIRE_EQ(result.result, h::ReceiverBridge::RequestState::confirmed);
}
TEST(hybrid_automatic_cursor_age_and_posting_delay_bound_the_original_operation)
{
    AutomaticFixture f(3, 100);
    f.candidate();
    f.provider->age = 50;
    const auto now = f.p.clock->value.load();
    f.controller->pump();
    REQUIRE_EQ(f.controller->snapshot().not_after_us, now + 50);
    f.p.clock->value = now + 50;
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::unknown);
    f.step();
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::unknown);
    REQUIRE(
        std::none_of(f.requests.begin(), f.requests.end(), [](const auto& m) {
            return m.kind == h::Kind::fallback_request;
        }));
}
TEST(hybrid_automatic_close_does_not_wait_for_a_running_cursor_callback)
{
    AutomaticFixture f;
    f.candidate();
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    f.provider->effect = [&](const auto&) {
        entered.set_value();
        gate.wait();
    };
    auto pumping = std::async(std::launch::async, [&] {
        f.controller->pump();
    });
    entered.get_future().wait();
    auto closing = std::async(std::launch::async, [&] {
        f.controller->close();
    });
    const auto ready =
        closing.wait_for(std::chrono::seconds {1}) == std::future_status::ready;
    release.set_value();
    closing.get();
    pumping.get();
    REQUIRE(ready);
    REQUIRE_EQ(f.controller->snapshot().admitted, 0U);
    REQUIRE(!f.receiver->snapshot().closed);
    REQUIRE(!f.provider->failed);
}
TEST(
    hybrid_automatic_lifetime_budget_allows_new_revisions_but_cannot_be_reset_by_flapping)
{
    for (std::uint32_t limit : {1U, 2U}) {
        AutomaticFixture f(limit);
        f.provider->available = false;
        f.candidate();
        f.controller->pump();
        for (std::uint64_t revision = 2; revision <= 2 * limit; revision += 2) {
            REQUIRE_EQ(f.receiver->request_fallback(1234),
                h::ReceiverBridge::Admission::accepted);
            f.step();
            auto response = f.response(h::Kind::delivery_status, revision,
                h::Mode::multicast, 1234 + revision / 2);
            response.outcome = h::Outcome::budget;
            f.deliver(response);
            f.progress(revision / 2, h::Activity::producing, revision);
            f.advance(10);
            f.progress(revision / 2 + 1, h::Activity::producing, revision);
            for (unsigned i = 0; i < 6; ++i)
                f.advance(10);
            REQUIRE(f.receiver->path_assessment()->fallback_candidate);
            f.controller->pump();
        }
        REQUIRE_EQ(f.provider->calls, limit);
        REQUIRE_EQ(f.controller->snapshot().operations, limit);
        REQUIRE(f.controller->snapshot().budget_exhausted);
        REQUIRE_EQ(f.controller->snapshot().phase,
            h::AutomaticFallback::Phase::exhausted);
    }
}
TEST(hybrid_automatic_eagain_without_submission_keeps_one_bounded_owned_attempt)
{
    AutomaticFixture f;
    f.candidate();
    f.p.ct->would_block = 100;
    f.controller->pump();
    for (unsigned i = 0; i < 60; ++i) {
        f.p.clock->value += 10;
        f.controller->pump();
        f.step();
    }
    REQUIRE_EQ(f.provider->calls, 1U);
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::unknown);
    REQUIRE_EQ(f.controller->snapshot().admitted, 1U);
    REQUIRE(
        std::none_of(f.requests.begin(), f.requests.end(), [](const auto& m) {
            return m.kind == h::Kind::fallback_request;
        }));
}
TEST(hybrid_automatic_new_timeout_does_not_inherit_a_prior_manual_denial)
{
    AutomaticFixture f;
    f.candidate();
    REQUIRE_EQ(f.receiver->request_fallback(1234),
        h::ReceiverBridge::Admission::accepted);
    f.step();
    auto denied = f.response();
    denied.outcome = h::Outcome::budget;
    f.deliver(denied);
    REQUIRE_EQ(f.receiver->snapshot().fallback_outcome, h::Outcome::budget);
    f.controller->pump();
    REQUIRE_EQ(f.controller->snapshot().admitted, 1U);
    for (unsigned i = 0; i < 60; ++i) {
        f.p.clock->value += 10;
        f.controller->pump();
        f.step();
    }
    const auto result = f.controller->snapshot();
    REQUIRE_EQ(result.result, h::ReceiverBridge::RequestState::unknown);
    REQUIRE_EQ(result.outcome, h::Outcome::ok);
    REQUIRE_EQ(f.provider->calls, 1U);
}
TEST(hybrid_automatic_total_loss_is_bounded_and_never_reinvokes_cursor_provider)
{
    AutomaticFixture f;
    f.candidate();
    f.controller->pump();
    for (unsigned i = 0; i < 60; ++i) {
        f.p.clock->value += 10;
        f.controller->pump();
        f.step();
    }
    REQUIRE_EQ(f.provider->calls, 1U);
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::unknown);
    const auto count =
        std::count_if(f.requests.begin(), f.requests.end(), [](const auto& m) {
            return m.kind == h::Kind::fallback_request;
        });
    REQUIRE(count > 0);
    REQUIRE(count <= 6);
    REQUIRE_EQ(f.controller->snapshot().admitted, 1U);
}
namespace {
struct AutomaticReturnFixture : ReceiverFixture {
    std::shared_ptr<h::AutomaticReturn> controller;
    std::uint64_t offset = 0, boundary = 0;
    static h::AutomaticReturn::Configuration policy()
    {
        return {2, 350, 4, 20, 100};
    }
    explicit AutomaticReturnFixture(
        h::AutomaticReturn::Configuration c = policy())
        : ReceiverFixture(AutomaticFixture::configuration(), true, true)
    {
        controller = h::AutomaticReturn::create(receiver, c);
        REQUIRE(controller);
    }
    void progress(h::Mode mode = h::Mode::unicast, std::uint64_t revision = 1,
        h::Activity activity = h::Activity::producing)
    {
        auto m =
            response(h::Kind::delivery_status, revision, mode, 1234 + offset);
        m.kind = h::Kind::stream_progress;
        m.request_serial = 0;
        m.multicast_next = m.next_position;
        m.produced_next = 2000;
        m.interval_us = 10;
        m.activity = activity;
        REQUIRE_EQ(
            p.l->send(m, p.clock->value.load() + 1000, p.clock->value.load()),
            h::NativeService::Admission::accepted);
        step();
        step();
    }
    void data(bool receive = true)
    {
        advance(10);
        compat::HybridAutomaticTestAccess::multicast(runtime, p.c,
            static_cast<std::uint32_t>(42 + offset), p.clock->value.load(),
            receive);
        ++offset;
    }
    void candidate(std::uint64_t revision = 1)
    {
        step();
        auto status = response(h::Kind::delivery_status, revision,
            h::Mode::unicast, 1234 + offset);
        status.multicast_next = 1234;
        deliver(status);
        progress(h::Mode::unicast, revision);
        for (unsigned i = 0; i < 10; ++i) {
            data();
            progress(h::Mode::unicast, revision);
        }
        const auto a = receiver->path_assessment();
        REQUIRE(a);
        REQUIRE(a->return_candidate);
    }
    void open()
    {
        candidate();
        controller->pump();
        step();
        REQUIRE_EQ(requests.back().kind, h::Kind::multicast_ready);
        boundary = 1234 + offset;
        deliver(
            response(h::Kind::delivery_commit, 2, h::Mode::overlap, boundary));
        REQUIRE_EQ(controller->snapshot().phase,
            h::AutomaticReturn::Phase::collecting);
    }
    void coverage(bool hole = false)
    {
        for (unsigned i = 0; i < 4; ++i) {
            data(!hole || i != 1);
            progress(h::Mode::overlap, 2);
        }
    }
    void commit()
    {
        controller->pump();
        step();
        REQUIRE_EQ(controller->snapshot().phase,
            h::AutomaticReturn::Phase::multicast_pending);
        deliver(response(
            h::Kind::delivery_commit, 3, h::Mode::multicast, 1234 + offset));
    }
};
}
TEST(hybrid_auto_return_factory_bounds_negotiation_detector_and_single_use)
{
    ReceiverFixture plain;
    REQUIRE(!h::AutomaticReturn::create(
        plain.receiver, AutomaticReturnFixture::policy()));
    ReceiverFixture negotiated(ReceiverFixture::configuration(), true, true);
    REQUIRE(!h::AutomaticReturn::create(
        negotiated.receiver, AutomaticReturnFixture::policy()));
    for (unsigned bad = 0; bad < 10; ++bad) {
        AutomaticReturnFixture f;
        auto c = f.policy();
        switch (bad) {
        case 0:
            c.maximum_operations = 0;
            break;
        case 1:
            c.maximum_operations = 9;
            break;
        case 2:
            c.operation_timeout_us = 0;
            break;
        case 3:
            c.operation_timeout_us = h::maximum_age_us + 1;
            break;
        case 4:
            c.coverage_packets = 1;
            break;
        case 5:
            c.coverage_packets = 33;
            break;
        case 6:
            c.minimum_coverage_us = 0;
            break;
        case 7:
            c.minimum_coverage_us = c.operation_timeout_us;
            break;
        case 8:
            c.proof_lifetime_us = 0;
            break;
        case 9:
            c.proof_lifetime_us = c.operation_timeout_us + 1;
            break;
        }
        REQUIRE(!h::AutomaticReturn::create(f.receiver, c));
        f.controller->close();
        REQUIRE(!h::AutomaticReturn::create(f.receiver, f.policy()));
    }
    AutomaticFixture legacy;
    REQUIRE(!h::AutomaticReturn::create(
        legacy.receiver, AutomaticReturnFixture::policy()));
    AutomaticReturnFixture f;
    auto provider = std::make_shared<AutomaticProvider>();
    provider->clock = f.p.clock;
    REQUIRE(h::AutomaticFallback::create(
        f.receiver, {1, 100}, {provider, AutomaticProvider::resolve}));
}
TEST(hybrid_auto_return_recovery_opens_overlap_then_requires_exact_coverage)
{
    AutomaticReturnFixture f;
    f.open();
    f.controller->pump();
    REQUIRE_EQ(f.controller->snapshot().multicast_admitted, 0U);
    f.coverage();
    f.commit();
    const auto s = f.controller->snapshot();
    REQUIRE_EQ(s.result, h::ReceiverBridge::RequestState::confirmed);
    REQUIRE_EQ(s.operations, 1U);
    REQUIRE_EQ(s.overlap_admitted, 1U);
    REQUIRE_EQ(s.multicast_admitted, 1U);
    REQUIRE_EQ(s.boundary, f.boundary);
    REQUIRE_EQ(s.through, f.boundary + 4);
    REQUIRE_EQ(
        f.receiver->snapshot().fallback, h::ReceiverBridge::RequestState::none);
}
TEST(hybrid_auto_return_sparse_high_water_does_not_prove_contiguous_coverage)
{
    AutomaticReturnFixture f;
    f.open();
    f.coverage(true);
    f.controller->pump();
    const auto progress = f.receiver->data_progress();
    REQUIRE(progress);
    REQUIRE_EQ(progress->multicast.next, f.boundary + 4);
    REQUIRE_EQ(f.controller->snapshot().multicast_admitted, 0U);
    f.p.clock->value = f.controller->snapshot().not_after_us;
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::unknown);
    f.controller->pump();
    REQUIRE_EQ(f.controller->snapshot().operations, 1U);
}
TEST(hybrid_auto_return_stale_proof_and_paused_source_preserve_overlap)
{
    for (bool stale : {false, true}) {
        AutomaticReturnFixture f;
        f.open();
        f.coverage();
        if (stale) {
            for (unsigned i = 0; i < 10; ++i)
                f.advance(10);
        } else
            f.progress(h::Mode::overlap, 2, h::Activity::paused);
        f.controller->pump();
        REQUIRE_EQ(f.controller->snapshot().multicast_admitted, 0U);
    }
}
TEST(
    hybrid_auto_return_manual_intent_after_overlap_wins_and_receipt_is_separate)
{
    AutomaticReturnFixture f;
    f.open();
    f.coverage();
    REQUIRE_EQ(f.receiver->request_fallback(f.boundary),
        h::ReceiverBridge::Admission::accepted);
    f.step();
    f.deliver(f.response(
        h::Kind::delivery_commit, 3, h::Mode::unicast, 1234 + f.offset));
    f.controller->pump();
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::unknown);
    REQUIRE_EQ(f.controller->snapshot().multicast_admitted, 0U);
    REQUIRE_EQ(f.receiver->snapshot().fallback,
        h::ReceiverBridge::RequestState::confirmed);
}
TEST(hybrid_auto_return_denial_does_not_rearm_same_unicast_revision)
{
    AutomaticReturnFixture f;
    f.candidate();
    f.controller->pump();
    f.step();
    auto m = f.response(
        h::Kind::delivery_status, 1, h::Mode::unicast, 1234 + f.offset);
    m.multicast_next = 1234;
    m.outcome = h::Outcome::budget;
    f.deliver(m);
    auto s = f.controller->snapshot();
    REQUIRE_EQ(s.result, h::ReceiverBridge::RequestState::denied);
    REQUIRE_EQ(s.outcome, h::Outcome::budget);
    for (unsigned i = 0; i < 10; ++i)
        f.controller->pump();
    REQUIRE_EQ(f.controller->snapshot().operations, 1U);
}
TEST(
    hybrid_auto_return_close_before_and_after_handoff_never_posts_another_phase)
{
    AutomaticReturnFixture early;
    early.candidate();
    early.controller->close();
    early.controller->pump();
    REQUIRE_EQ(early.controller->snapshot().overlap_admitted, 0U);
    AutomaticReturnFixture overlap;
    overlap.candidate();
    overlap.controller->pump();
    overlap.controller->close();
    overlap.step();
    overlap.deliver(
        overlap.response(h::Kind::delivery_commit, 2, h::Mode::overlap, 1244));
    overlap.controller->pump();
    REQUIRE_EQ(overlap.controller->snapshot().multicast_admitted, 0U);
    REQUIRE(!overlap.receiver->snapshot().closed);
    AutomaticReturnFixture final;
    final.open();
    final.coverage();
    final.controller->pump();
    final.controller->close();
    final.step();
    final.deliver(
        final.response(h::Kind::delivery_commit, 3, h::Mode::multicast, 1248));
    REQUIRE_EQ(final.controller->snapshot().result,
        h::ReceiverBridge::RequestState::confirmed);
    REQUIRE(final.controller->snapshot().closed);
}
TEST(hybrid_auto_return_total_loss_and_posting_delay_keep_original_deadline)
{
    for (bool delay : {false, true}) {
        AutomaticReturnFixture f;
        f.candidate();
        f.controller->pump();
        const auto deadline = f.controller->snapshot().not_after_us;
        if (delay)
            f.p.clock->value = deadline;
        else
            for (unsigned i = 0; i < 35; ++i) {
                f.advance(10);
                f.controller->pump();
            }
        REQUIRE_EQ(f.controller->snapshot().result,
            h::ReceiverBridge::RequestState::unknown);
        REQUIRE_EQ(f.controller->snapshot().operations, 1U);
        REQUIRE_EQ(f.controller->snapshot().not_after_us, deadline);
        REQUIRE_EQ(f.controller->snapshot().multicast_admitted, 0U);
    }
}
TEST(hybrid_auto_return_lost_commits_recover_exact_boundary_via_queries)
{
    auto policy = AutomaticReturnFixture::policy();
    policy.proof_lifetime_us = 200;
    AutomaticReturnFixture f(policy);
    f.candidate();
    f.controller->pump();
    f.step();
    f.boundary = 1234 + f.offset;
    f.advance(100);
    REQUIRE_EQ(f.requests.back().kind, h::Kind::delivery_query);
    auto m = f.response(
        h::Kind::delivery_status, 2, h::Mode::overlap, f.boundary + 4);
    m.multicast_next = f.boundary;
    f.deliver(m);
    REQUIRE_EQ(f.controller->snapshot().boundary, f.boundary);
    f.coverage();
    f.controller->pump();
    f.step();
    f.advance(100);
    REQUIRE_EQ(f.requests.back().kind, h::Kind::delivery_query);
    m = f.response(
        h::Kind::delivery_status, 3, h::Mode::multicast, f.boundary + 7);
    m.multicast_next = f.boundary + 6;
    f.deliver(m);
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::confirmed);
    REQUIRE_EQ(f.controller->snapshot().through, f.boundary + 4);
    REQUIRE_EQ(f.controller->snapshot().commit_position, f.boundary + 6);
}
TEST(hybrid_auto_return_lifetime_budget_cannot_be_reset_by_new_uc_revisions)
{
    auto policy = AutomaticReturnFixture::policy();
    policy.maximum_operations = 1;
    AutomaticReturnFixture f(policy);
    f.candidate();
    f.controller->pump();
    f.step();
    auto m = f.response(
        h::Kind::delivery_status, 1, h::Mode::unicast, 1234 + f.offset);
    m.multicast_next = 1234;
    m.outcome = h::Outcome::budget;
    f.deliver(m);
    f.advance(200);
    f.candidate(3);
    f.controller->pump();
    REQUIRE_EQ(f.controller->snapshot().operations, 1U);
    REQUIRE(f.controller->snapshot().budget_exhausted);
    REQUIRE_EQ(
        f.controller->snapshot().phase, h::AutomaticReturn::Phase::exhausted);
}
TEST(hybrid_auto_return_route_loss_and_native_close_prevent_final_admission)
{
    for (bool close : {false, true}) {
        AutomaticReturnFixture f;
        f.open();
        f.coverage();
        if (close)
            f.receiver->close();
        else
            f.multicast->unregister_connection(
                f.p.c->origin()->dispatch().group_id);
        f.controller->pump();
        REQUIRE_EQ(f.controller->snapshot().multicast_admitted, 0U);
        if (close)
            REQUIRE(f.controller->snapshot().closed);
        else {
            f.advance(350);
            REQUIRE_EQ(f.controller->snapshot().result,
                h::ReceiverBridge::RequestState::unknown);
        }
    }
}
TEST(hybrid_auto_return_deadline_is_clipped_to_receiver_bound)
{
    auto p = AutomaticReturnFixture::policy();
    p.operation_timeout_us = 1000;
    AutomaticReturnFixture f(p);
    f.candidate();
    const auto started = f.p.clock->value.load();
    f.controller->pump();
    REQUIRE_EQ(f.controller->snapshot().not_after_us, started + 400);
    f.p.clock->value = started + 400;
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::unknown);
}
TEST(hybrid_auto_return_matching_late_mc_copy_fills_the_exact_coverage_hole)
{
    AutomaticReturnFixture f;
    f.open();
    f.coverage(true);
    f.controller->pump();
    REQUIRE_EQ(f.controller->snapshot().multicast_admitted, 0U);
    compat::HybridAutomaticTestAccess::multicast(f.runtime, f.p.c,
        static_cast<std::uint32_t>(42 + f.boundary - 1234 + 1),
        f.p.clock->value.load());
    f.commit();
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::confirmed);
}
TEST(hybrid_auto_return_new_uc_revision_consumes_next_lifetime_allowance)
{
    AutomaticReturnFixture f;
    f.candidate();
    f.controller->pump();
    f.step();
    auto m = f.response(
        h::Kind::delivery_status, 1, h::Mode::unicast, 1234 + f.offset);
    m.multicast_next = 1234;
    m.outcome = h::Outcome::budget;
    f.deliver(m);
    f.advance(200);
    f.candidate(3);
    f.controller->pump();
    REQUIRE_EQ(f.controller->snapshot().operations, 2U);
    REQUIRE_EQ(f.controller->snapshot().revision, 3U);
    REQUIRE_EQ(f.controller->snapshot().overlap_admitted, 2U);
    REQUIRE(f.controller->snapshot().budget_exhausted);
}
TEST(hybrid_auto_return_multicast_denial_receipt_survives_later_manual_outcome)
{
    AutomaticReturnFixture f;
    f.open();
    f.coverage();
    f.controller->pump();
    f.step();
    auto m = f.response(
        h::Kind::delivery_status, 2, h::Mode::overlap, 1234 + f.offset);
    m.multicast_next = f.boundary;
    m.outcome = h::Outcome::history_unavailable;
    f.deliver(m);
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::denied);
    REQUIRE_EQ(
        f.controller->snapshot().outcome, h::Outcome::history_unavailable);
    REQUIRE_EQ(f.receiver->request_fallback(f.boundary),
        h::ReceiverBridge::Admission::accepted);
    f.step();
    f.deliver(f.response(
        h::Kind::delivery_commit, 3, h::Mode::unicast, 1234 + f.offset));
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::denied);
    REQUIRE_EQ(
        f.controller->snapshot().outcome, h::Outcome::history_unavailable);
}
TEST(hybrid_auto_return_native_grant_clips_handoff_and_expires_pending_receipt)
{
    AutomaticReturnFixture f;
    const auto expiry = f.p.c->snapshot().expires_at_us;
    f.p.clock->value = expiry - 150;
    f.candidate();
    f.controller->pump();
    REQUIRE_EQ(f.controller->snapshot().overlap_admitted, 1U);
    REQUIRE_EQ(f.controller->snapshot().not_after_us, expiry);
    f.p.clock->value = expiry;
    REQUIRE_EQ(f.controller->snapshot().result,
        h::ReceiverBridge::RequestState::unknown);
    REQUIRE(f.controller->snapshot().closed);
}
namespace {
struct MembershipProvider {
    std::function<void(const h::MembershipRecovery::Request&)> effect;
    unsigned calls = 0;
    bool failed = false, succeed = false;
    static std::optional<UdpSocket> prepare(
        void* p, const h::MembershipRecovery::Request& r) noexcept
    {
        auto& self = *static_cast<MembershipProvider*>(p);
        ++self.calls;
        try {
            if (self.effect)
                self.effect(r);
        } catch (...) {
            self.failed = true;
        }
        if (self.succeed) {
            UdpSocket socket(r.context.dispatch.multicast_group.family);
            if (socket.valid() && socket.set_reuse_address(true) == Error::none
                && socket.bind(
                       IpEndpoint::any(r.context.dispatch.multicast_group.port))
                    == Error::none)
                return socket; // Injected provider contract; no real MC traffic in components.
        }
        return {};
    }
};
struct MembershipFixture : ReceiverFixture {
    std::shared_ptr<MembershipProvider> provider =
        std::make_shared<MembershipProvider>();
    std::shared_ptr<h::MembershipRecovery> membership;
    std::shared_ptr<compat::DatagramChannel> control =
        std::make_shared<compat::DatagramChannel>();
    static h::MembershipRecovery::Configuration policy()
    {
        return {IpEndpoint::any(), 3, 400, 10, 40};
    }
    MembershipFixture()
        : ReceiverFixture(AutomaticFixture::configuration(), true, true, true)
    {
        multicast->unregister_connection(p.c->origin()->dispatch().group_id);
        REQUIRE(!receiver->data_progress());
        REQUIRE(
            control->register_ingress_route(p.c->origin()->dispatch().group_id,
                runtime, compat::PacketIngress::unicast_session));
        compat::HybridAutomaticTestAccess::channel(runtime, control);
    }
    void attach(h::MembershipRecovery::Configuration c = policy())
    {
        membership = h::MembershipRecovery::create(
            p.c, c, {provider, MembershipProvider::prepare}, receiver);
        REQUIRE(membership);
    }
};
}
TEST(
    hybrid_membership_factory_requires_negotiation_absence_and_empty_receive_history)
{
    ReceiverFixture legacy(ReceiverFixture::configuration(), true, true);
    legacy.multicast->unregister_connection(
        legacy.p.c->origin()->dispatch().group_id);
    REQUIRE(!h::MembershipRecovery::create(
        legacy.p.c, MembershipFixture::policy()));
    for (unsigned bad = 0; bad < 8; ++bad) {
        MembershipFixture f;
        auto c = f.policy();
        switch (bad) {
        case 0:
            c.maximum_attempts = 0;
            break;
        case 1:
            c.maximum_attempts = 9;
            break;
        case 2:
            c.timeout_us = 0;
            break;
        case 3:
            c.timeout_us = h::maximum_age_us + 1;
            break;
        case 4:
            c.initial_backoff_us = 0;
            break;
        case 5:
            c.initial_backoff_us = 41;
            break;
        case 6:
            c.interface.port = 1;
            break;
        case 7:
            c.maximum_backoff_us = h::maximum_interval_us + 1;
            break;
        }
        REQUIRE(!h::MembershipRecovery::create(f.p.c, c));
    }
    ReceiverFixture joined(ReceiverFixture::configuration(), true, true, true);
    REQUIRE(!h::MembershipRecovery::create(
        joined.p.c, MembershipFixture::policy()));
    MembershipFixture absent;
    absent.control->unregister_connection(
        absent.p.c->origin()->dispatch().group_id);
    REQUIRE(!h::MembershipRecovery::create(absent.p.c, absent.policy()));
    MembershipFixture data;
    compat::HybridAutomaticTestAccess::multicast(
        data.runtime, data.p.c, 42, data.p.clock->value.load(), false);
    REQUIRE(
        !h::MembershipRecovery::create(data.p.c, MembershipFixture::policy()));
}
TEST(hybrid_membership_backoff_attempts_and_single_use_are_bounded)
{
    MembershipFixture f;
    f.attach();
    f.membership->pump();
    REQUIRE_EQ(f.provider->calls, 1U);
    REQUIRE_EQ(
        f.membership->snapshot().next_wakeup_us, f.p.clock->value.load() + 10);
    for (unsigned i = 0; i < 10; ++i)
        f.membership->pump();
    REQUIRE_EQ(f.provider->calls, 1U);
    f.advance(10);
    f.membership->pump();
    REQUIRE_EQ(f.provider->calls, 2U);
    f.advance(19);
    f.membership->pump();
    REQUIRE_EQ(f.provider->calls, 2U);
    f.advance(1);
    f.membership->pump();
    REQUIRE_EQ(f.provider->calls, 3U);
    REQUIRE_EQ(f.membership->snapshot().phase,
        h::MembershipRecovery::Phase::exhausted);
    REQUIRE(f.membership->snapshot().budget_exhausted);
    REQUIRE(f.receiver->data_progress());
    f.membership->close();
    REQUIRE(!f.receiver->data_progress());
    REQUIRE(!h::MembershipRecovery::create(f.p.c, f.policy()));
}
TEST(hybrid_membership_absence_records_valid_uc_without_fabricating_mc_coverage)
{
    MembershipFixture f;
    f.attach();
    compat::HybridAutomaticTestAccess::multicast(
        f.runtime, f.p.c, 42, f.p.clock->value.load(), false);
    auto progress = f.receiver->data_progress();
    REQUIRE(progress);
    REQUIRE_EQ(progress->all.next, 1235U);
    REQUIRE_EQ(progress->unicast.next, 1235U);
    REQUIRE(!progress->multicast.observed);
    REQUIRE(!f.receiver->coverage_proof(1235, 100));
    auto rogue = std::make_shared<compat::DatagramChannel>();
    REQUIRE(!rogue->register_ingress_route(f.p.c->origin()->dispatch().group_id,
        f.runtime, compat::PacketIngress::multicast_data,
        compat::MulticastIngressConfiguration {f.p.c->origin()->source()}));
    f.membership->close();
    REQUIRE(!f.receiver->data_progress());
}
TEST(hybrid_membership_deadline_and_provider_latency_cannot_refresh_the_budget)
{
    for (bool during : {false, true}) {
        MembershipFixture f;
        f.attach();
        const auto deadline = f.membership->snapshot().not_after_us;
        if (during)
            f.provider->effect = [&](const auto&) {
                f.p.clock->value = deadline;
            };
        else
            f.p.clock->value = deadline;
        f.membership->pump();
        REQUIRE_EQ(f.membership->snapshot().phase,
            h::MembershipRecovery::Phase::expired);
        REQUIRE_EQ(f.membership->snapshot().not_after_us, deadline);
        REQUIRE_EQ(f.provider->calls, during ? 1U : 0U);
        for (unsigned i = 0; i < 10; ++i)
            f.membership->pump();
        REQUIRE_EQ(f.provider->calls, during ? 1U : 0U);
    }
}
TEST(hybrid_membership_provider_can_inspect_reenter_or_close_outside_locks)
{
    for (bool close : {false, true}) {
        MembershipFixture f;
        f.attach();
        f.provider->effect = [&](const auto& r) {
            REQUIRE_EQ(r.context, f.p.c->snapshot().context);
            REQUIRE_EQ(r.generation, f.p.c->snapshot().generation);
            REQUIRE_EQ(r.attempt, 1U);
            REQUIRE_EQ(f.membership->snapshot().phase,
                h::MembershipRecovery::Phase::preparing);
            f.membership->pump();
            if (close)
                f.membership->close();
        };
        f.membership->pump();
        REQUIRE(!f.provider->failed);
        REQUIRE_EQ(f.provider->calls, 1U);
        REQUIRE_EQ(f.membership->snapshot().closed, close);
        REQUIRE(!f.receiver->snapshot().closed);
    }
}
TEST(hybrid_membership_close_does_not_wait_for_a_running_provider)
{
    MembershipFixture f;
    f.attach();
    std::promise<void> entered, release;
    auto ready = release.get_future().share();
    f.provider->effect = [&](const auto&) {
        entered.set_value();
        ready.wait();
    };
    auto running = std::async(std::launch::async, [&] {
        f.membership->pump();
    });
    entered.get_future().wait();
    f.membership->close();
    REQUIRE(f.membership->snapshot().closed);
    release.set_value();
    running.get();
    REQUIRE_EQ(f.provider->calls, 1U);
    REQUIRE(!f.membership->snapshot().joined);
}
TEST(
    hybrid_membership_native_expiry_clock_regression_and_close_hide_observations)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        MembershipFixture f;
        f.attach();
        if (mode == 0)
            f.p.clock->value = f.p.c->snapshot().expires_at_us;
        if (mode == 1)
            --f.p.clock->value;
        if (mode == 2)
            f.receiver->close();
        REQUIRE(f.membership->snapshot().closed);
        f.membership->pump();
        REQUIRE(!f.receiver->data_progress());
        REQUIRE_EQ(f.provider->calls, 0U);
    }
}
TEST(
    hybrid_membership_startup_outage_uses_trusted_initial_cursor_not_sender_horizon)
{
    for (bool bad_cursor : {false, true}) {
        MembershipFixture f;
        f.attach();
        f.step();
        f.deliver(f.response());
        const auto report = [&](std::uint64_t offset) {
            auto m = f.response(
                h::Kind::delivery_status, 0, h::Mode::multicast, 1234 + offset);
            m.kind = h::Kind::stream_progress;
            m.request_serial = 0;
            m.multicast_next = m.next_position;
            m.produced_next = 1334;
            m.interval_us = 10;
            m.activity = h::Activity::producing;
            REQUIRE_EQ(f.p.l->send(m, f.p.clock->value.load() + 1000,
                           f.p.clock->value.load()),
                h::NativeService::Admission::accepted);
            f.step();
            f.step();
        };
        report(0);
        f.advance(10);
        report(1);
        for (unsigned i = 0; i < 6; ++i)
            f.advance(10);
        REQUIRE(f.receiver->path_assessment()->fallback_candidate);
        auto provider = std::make_shared<AutomaticProvider>();
        provider->clock = f.p.clock;
        if (bad_cursor)
            provider->position = 1235;
        auto controller = h::AutomaticFallback::create(
            f.receiver, {1, 500}, {provider, AutomaticProvider::resolve});
        REQUIRE(controller);
        controller->pump();
        REQUIRE_EQ(provider->calls, 1U);
        REQUIRE_EQ(controller->snapshot().admitted, bad_cursor ? 0U : 1U);
        if (!bad_cursor) {
            f.step();
            f.deliver(f.response(
                h::Kind::delivery_commit, 1, h::Mode::unicast, 1235));
            REQUIRE_EQ(controller->snapshot().result,
                h::ReceiverBridge::RequestState::confirmed);
            REQUIRE_EQ(controller->snapshot().position, 1234U);
        }
        REQUIRE(!f.receiver->data_progress()->multicast.observed);
    }
}
TEST(
    hybrid_membership_grant_expiry_clips_retry_deadline_and_rejects_incomplete_provider)
{
    MembershipFixture f;
    REQUIRE(!h::MembershipRecovery::create(
        f.p.c, f.policy(), {f.provider, nullptr}));
    const auto expiry = f.p.c->snapshot().expires_at_us;
    f.p.clock->value = expiry - 50;
    f.attach();
    REQUIRE_EQ(f.membership->snapshot().not_after_us, expiry);
    f.p.clock->value = expiry;
    REQUIRE(f.membership->snapshot().closed);
    f.membership->pump();
    REQUIRE_EQ(f.provider->calls, 0U);
}
TEST(hybrid_membership_absence_does_not_admit_foreign_uc_or_unbound_mc)
{
    MembershipFixture f;
    f.attach();
    const std::array<std::byte, 1> payload {std::byte {1}};
    const PacketView packet {.kind = PacketKind::data,
        .data = {.sequence = SequenceNumber {42},
            .message_number = 1,
            .boundary = MessageBoundary::solo,
            .destination_socket_id = f.p.c->origin()->dispatch().group_id},
        .payload = payload};
    f.runtime->process_packet(packet, IpEndpoint::loopback(1));
    f.runtime->process_packet(packet, f.p.c->origin()->source().endpoint,
        std::nullopt, compat::PacketIngress::multicast_data);
    const auto progress = f.receiver->data_progress();
    REQUIRE(progress);
    REQUIRE(!progress->all.observed);
}
namespace {
struct ReturnFixture : ReceiverFixture {
    ReturnFixture()
        : ReceiverFixture(configuration(), true, true)
    {
        step();
        deliver(response(h::Kind::delivery_status, 1, h::Mode::unicast));
        REQUIRE(receiver->snapshot().fresh);
        REQUIRE_EQ(receiver->snapshot().mode, h::Mode::unicast);
    }
    void overlap()
    {
        REQUIRE_EQ(receiver->request_overlap(),
            h::ReceiverBridge::Admission::accepted);
        step();
        deliver(response(h::Kind::delivery_commit, 2, h::Mode::overlap));
        REQUIRE_EQ(receiver->snapshot().returning,
            h::ReceiverBridge::RequestState::confirmed);
    }
    h::CoverageProof coverage()
    {
        const auto now = p.clock->value.load();
        compat::HybridAutomaticTestAccess::multicast(runtime, p.c, 42, now);
        compat::HybridAutomaticTestAccess::multicast(runtime, p.c, 43, now);
        REQUIRE_EQ(receiver->data_progress()->multicast.next, 1236U);
        return {.generation = p.c->snapshot().generation,
            .revision = 2,
            .boundary = 1234,
            .through = 1236,
            .observed_at_us = now,
            .not_after_us = now + 100,
            .interval_us = 10};
    }
};
}
TEST(
    hybrid_return_receiver_requires_live_negotiated_uc_and_exact_scoped_coverage)
{
    ReceiverFixture legacy;
    REQUIRE_EQ(legacy.receiver->request_overlap(),
        h::ReceiverBridge::Admission::invalid);
    ReturnFixture f;
    f.overlap();
    auto proof = f.coverage();
    for (unsigned wrong = 0; wrong < 5; ++wrong) {
        auto bad = proof;
        if (wrong == 0)
            bad.boundary = 1235;
        if (wrong == 1)
            bad.revision = 1;
        if (wrong == 2)
            bad.through = 1237;
        if (wrong == 3)
            bad.generation = 99;
        if (wrong == 4)
            bad.not_after_us = f.p.clock->value.load();
        REQUIRE_EQ(f.receiver->request_multicast(bad),
            h::ReceiverBridge::Admission::invalid);
    }
    REQUIRE_EQ(f.receiver->request_multicast(proof),
        h::ReceiverBridge::Admission::accepted);
    REQUIRE_EQ(
        f.receiver->request_fallback(1234), h::ReceiverBridge::Admission::busy);
    f.step();
    REQUIRE_EQ(f.requests.back().kind, h::Kind::multicast_ready);
    REQUIRE_EQ(f.requests.back().multicast_next, 1234U);
    f.deliver(
        f.response(h::Kind::delivery_commit, 3, h::Mode::multicast, 1236));
    REQUIRE_EQ(f.receiver->snapshot().mode, h::Mode::multicast);
    REQUIRE_EQ(f.receiver->snapshot().returning,
        h::ReceiverBridge::RequestState::confirmed);
}
TEST(
    hybrid_return_receiver_lost_commit_query_recovers_boundary_not_admission_cursor)
{
    ReturnFixture f;
    REQUIRE_EQ(
        f.receiver->request_overlap(), h::ReceiverBridge::Admission::accepted);
    f.step();
    f.advance(100);
    REQUIRE_EQ(f.requests.back().kind, h::Kind::delivery_query);
    auto status =
        f.response(h::Kind::delivery_status, 2, h::Mode::overlap, 1236);
    status.multicast_next = 1234;
    f.deliver(status);
    const auto s = f.receiver->snapshot();
    REQUIRE(s.fresh);
    REQUIRE(s.has_commit);
    REQUIRE_EQ(s.cursor, 1236U);
    REQUIRE_EQ(s.commit_position, 1234U);
    REQUIRE_EQ(s.returning, h::ReceiverBridge::RequestState::confirmed);
}
TEST(
    hybrid_return_receiver_timed_ready_retries_use_fresh_serials_and_original_observation)
{
    ReturnFixture f;
    REQUIRE_EQ(
        f.receiver->request_overlap(), h::ReceiverBridge::Admission::accepted);
    f.step();
    const auto first = f.requests.back();
    f.advance(10);
    const auto second = f.requests.back();
    REQUIRE_EQ(second.kind, h::Kind::multicast_ready);
    REQUIRE(second.serial > first.serial);
    REQUIRE_EQ(second.revision, first.revision);
    REQUIRE_EQ(second.next_position, first.next_position);
    REQUIRE(second.age_us >= 10U);
    REQUIRE_EQ(f.receiver->snapshot().request_serial, second.serial);
    f.deliver(f.response(h::Kind::delivery_commit, 2, h::Mode::overlap));
    REQUIRE_EQ(f.receiver->snapshot().returning,
        h::ReceiverBridge::RequestState::confirmed);
}
TEST(hybrid_return_receiver_expired_overlap_and_total_loss_never_confirm_return)
{
    ReturnFixture f;
    f.overlap();
    auto proof = f.coverage();
    REQUIRE_EQ(f.receiver->request_multicast(proof),
        h::ReceiverBridge::Admission::accepted);
    f.step();
    auto expired =
        f.response(h::Kind::delivery_status, 3, h::Mode::unicast, 1236);
    expired.outcome = h::Outcome::stale;
    f.deliver(expired);
    REQUIRE_EQ(f.receiver->snapshot().returning,
        h::ReceiverBridge::RequestState::denied);
    REQUIRE_EQ(f.receiver->snapshot().mode, h::Mode::unicast);
    ReturnFixture lost;
    REQUIRE_EQ(lost.receiver->request_overlap(),
        h::ReceiverBridge::Admission::accepted);
    for (unsigned i = 0; i < 60; ++i)
        lost.advance(10);
    REQUIRE_EQ(lost.receiver->snapshot().returning,
        h::ReceiverBridge::RequestState::unknown);
    REQUIRE_EQ(lost.receiver->snapshot().fallback,
        h::ReceiverBridge::RequestState::none);
}
TEST(hybrid_return_receiver_conflicting_same_revision_boundary_closes_control)
{
    ReturnFixture f;
    f.overlap();
    auto conflict =
        f.response(h::Kind::delivery_status, 2, h::Mode::overlap, 1236);
    conflict.multicast_next = 1235;
    f.deliver(conflict);
    REQUIRE(f.receiver->snapshot().closed);
}
TEST(
    hybrid_detector_receiver_opt_in_is_validated_and_never_creates_fallback_without_data)
{
    ReceiverFixture disabled;
    REQUIRE(!disabled.receiver->path_assessment());
    auto c = ReceiverFixture::configuration();
    c.detection = h::DetectionConfiguration {};
    Pair invalid;
    invalid.connect();
    REQUIRE(!h::ReceiverBridge::create(invalid.c, c));
    c.detection = h::DetectionConfiguration {.report_age_us = 1000,
        .maximum_transit_us = 10,
        .stall_us = 50,
        .emission_lifetime_us = 300,
        .recovery_us = 40,
        .recovery_gap_us = 30,
        .hold_down_us = 80,
        .sample_gap_us = 20,
        .recovery_advances = 3};
    ReceiverFixture f(c);
    for (unsigned i = 0; i < 30; ++i) {
        f.p.clock->value += 10;
        f.step();
    }
    const auto assessment = f.receiver->path_assessment();
    REQUIRE(assessment);
    REQUIRE_EQ(assessment->state, h::PathDetector::State::unknown);
    REQUIRE_EQ(assessment->reason, h::PathDetector::Reason::no_data);
    REQUIRE(!assessment->fallback_candidate);
    REQUIRE(
        std::all_of(f.requests.begin(), f.requests.end(), [](const auto& m) {
            return m.kind == h::Kind::delivery_query;
        }));
    REQUIRE_EQ(
        f.receiver->snapshot().fallback, h::ReceiverBridge::RequestState::none);
    REQUIRE(f.receiver->snapshot().next_wakeup_us <= f.p.clock->value + 20);
    f.receiver->close();
    REQUIRE(!f.receiver->path_assessment());
}
TEST(
    hybrid_reporting_receiver_consumes_authenticated_stream_reports_without_policy_confirmation)
{
    ReceiverFixture f;
    auto m = f.response();
    m.kind = h::Kind::stream_progress;
    m.request_serial = 0;
    m.produced_next = 1240;
    m.retained_floor = 1234;
    m.multicast_next = 1234;
    m.interval_us = 100;
    m.activity = h::Activity::paused;
    REQUIRE_EQ(
        f.p.l->send(m, 1100, 100), h::NativeService::Admission::accepted);
    f.step();
    f.step();
    auto observed = f.receiver->peer_report(150);
    REQUIRE(observed);
    REQUIRE(observed->fresh);
    REQUIRE_EQ(observed->message.activity, h::Activity::paused);
    REQUIRE_EQ(observed->next_advanced_at_us, 0U);
    REQUIRE(!f.receiver->snapshot().observed);
    REQUIRE(!f.receiver->snapshot().fresh);
    f.p.clock->value += 160;
    REQUIRE(!f.receiver->peer_report(150)->fresh);
    REQUIRE_EQ(
        f.receiver->snapshot().fallback, h::ReceiverBridge::RequestState::none);
    f.receiver->close();
    REQUIRE(!f.receiver->peer_report(150));
}
TEST(
    hybrid_reporting_receiver_without_native_data_observer_does_not_invent_path_reports)
{
    auto configuration = ReceiverFixture::configuration();
    configuration.reporting = {10, 100};
    ReceiverFixture f(configuration);
    for (unsigned i = 0; i < 20; ++i) {
        f.p.clock->value += 10;
        f.step();
    }
    REQUIRE_EQ(f.receiver->snapshot().reports_queued, 0U);
    REQUIRE(f.receiver->snapshot().reports_skipped > 0);
    REQUIRE(
        std::none_of(f.requests.begin(), f.requests.end(), [](const auto& m) {
            return m.kind == h::Kind::path_report;
        }));
}
TEST(hybrid_receiver_queries_confirm_and_expire_without_requesting_fallback)
{
    ReceiverFixture f;
    f.step();
    REQUIRE_EQ(f.requests.back().kind, h::Kind::delivery_query);
    f.deliver(f.response());
    auto s = f.receiver->snapshot();
    REQUIRE(s.fresh);
    REQUIRE_EQ(s.phase, h::ReceiverBridge::Phase::known);
    f.advance(200);
    REQUIRE(!f.receiver->snapshot().fresh);
    REQUIRE_EQ(f.requests.back().kind, h::Kind::delivery_query);
    f.deliver(
        f.response(h::Kind::delivery_status, 0, h::Mode::multicast, 1235));
    REQUIRE(f.receiver->snapshot().fresh);
    REQUIRE_EQ(f.receiver->snapshot().cursor, 1235U);
    for (const auto& request : f.requests)
        REQUIRE_EQ(request.kind, h::Kind::delivery_query);
}
TEST(hybrid_receiver_explicit_fallback_preserves_cursor_and_tracks_commit)
{
    ReceiverFixture f;
    REQUIRE_EQ(f.receiver->request_fallback(1240),
        h::ReceiverBridge::Admission::accepted);
    f.step();
    REQUIRE_EQ(f.requests.back().kind, h::Kind::fallback_request);
    REQUIRE_EQ(f.requests.back().next_position, 1240U);
    f.deliver(f.response(h::Kind::delivery_commit, 1, h::Mode::unicast, 1241));
    const auto s = f.receiver->snapshot();
    REQUIRE(s.fresh);
    REQUIRE(s.has_commit);
    REQUIRE_EQ(s.commit_position, 1241U);
    REQUIRE_EQ(s.revision, 1U);
    REQUIRE_EQ(f.receiver->request_fallback(1241),
        h::ReceiverBridge::Admission::already_unicast);
}
TEST(hybrid_receiver_lost_commit_reconciles_by_query_without_replaying_fallback)
{
    ReceiverFixture f;
    REQUIRE_EQ(f.receiver->request_fallback(1234),
        h::ReceiverBridge::Admission::accepted);
    f.step();
    const auto old = f.requests.back().serial;
    f.advance(100);
    REQUIRE_EQ(f.requests.back().kind, h::Kind::delivery_query);
    auto current =
        f.response(h::Kind::delivery_status, 1, h::Mode::unicast, 1238);
    current.serial = 2;
    current.retained_floor = 1236;
    f.deliver(current);
    const auto confirmed = f.receiver->snapshot();
    REQUIRE(confirmed.fresh);
    REQUIRE(!confirmed.has_commit);
    f.advance(10);
    auto late =
        f.response(h::Kind::delivery_commit, 1, h::Mode::unicast, 1237, old);
    late.serial = 1;
    f.deliver(late);
    const auto after = f.receiver->snapshot();
    REQUIRE_EQ(after.confirmation_origin_us, confirmed.confirmation_origin_us);
    REQUIRE_EQ(after.cursor, 1238U);
    REQUIRE_EQ(after.retained_floor, 1236U);
    REQUIRE(after.has_commit);
    REQUIRE_EQ(after.commit_position, 1237U);
}
TEST(
    hybrid_receiver_old_revisions_never_roll_back_and_same_revision_conflicts_close)
{
    for (bool boundary : {false, true}) {
        ReceiverFixture f;
        REQUIRE_EQ(f.receiver->request_fallback(1234),
            h::ReceiverBridge::Admission::accepted);
        f.step();
        f.deliver(
            f.response(h::Kind::delivery_commit, 2, h::Mode::unicast, 1237));
        f.deliver(f.response(h::Kind::delivery_status, 1, h::Mode::multicast));
        REQUIRE_EQ(f.receiver->snapshot().revision, 2U);
        REQUIRE_EQ(f.receiver->snapshot().mode, h::Mode::unicast);
        f.deliver(f.response(
            boundary ? h::Kind::delivery_commit : h::Kind::delivery_status, 2,
            boundary ? h::Mode::unicast : h::Mode::multicast,
            boundary ? 1238 : 1237));
        REQUIRE(f.receiver->snapshot().closed);
        REQUIRE_EQ(f.receiver->snapshot().conflicts, 1U);
    }
    ReceiverFixture backwards;
    backwards.step();
    backwards.deliver(backwards.response(
        h::Kind::delivery_status, 0, h::Mode::multicast, 1240));
    backwards.deliver(backwards.response(
        h::Kind::delivery_commit, 1, h::Mode::unicast, 1239));
    REQUIRE(backwards.receiver->snapshot().closed);
}
TEST(
    hybrid_receiver_uncorrelated_or_stale_queued_replies_cannot_confirm_freshness)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        auto c = ReceiverFixture::configuration();
        if (mode == 2)
            c.confirmation_lifetime_us = 50;
        ReceiverFixture f(c);
        f.step();
        auto m = f.response();
        if (mode < 2) {
            m.request_serial = mode == 0 ? 0 : 77;
            f.deliver(m);
        } else {
            REQUIRE_EQ(
                f.p.l->send(m, 1000), h::NativeService::Admission::accepted);
            f.p.round();
            f.p.round();
            f.p.clock->value += 60;
            f.receiver->pump();
        }
        REQUIRE(!f.receiver->snapshot().fresh);
        REQUIRE(f.receiver->snapshot().unmatched > 0);
    }
}
TEST(hybrid_receiver_retry_rounds_and_lifetimes_are_bounded_under_total_loss)
{
    ReceiverFixture f;
    f.step();
    f.advance(10);
    f.advance(10);
    REQUIRE_EQ(f.receiver->snapshot().attempts, 2U);
    f.advance(80);
    f.advance(10);
    f.advance(90);
    f.advance(10);
    f.advance(90);
    const auto s = f.receiver->snapshot();
    REQUIRE_EQ(s.phase, h::ReceiverBridge::Phase::unknown);
    REQUIRE_EQ(s.attempts, 6U);
    REQUIRE_EQ(s.rounds, 3U);
    REQUIRE_EQ(s.exhausted, 1U);
    for (unsigned i = 0; i < 16; ++i)
        f.step();
    REQUIRE_EQ(f.receiver->snapshot().attempts, 6U);
    for (const auto& request : f.requests)
        REQUIRE_EQ(request.kind, h::Kind::delivery_query);
}
TEST(
    hybrid_receiver_query_after_lost_request_can_retry_explicit_intent_but_stops_on_denial)
{
    ReceiverFixture f;
    REQUIRE_EQ(f.receiver->request_fallback(1240),
        h::ReceiverBridge::Admission::accepted);
    f.step();
    f.advance(100);
    REQUIRE_EQ(f.requests.back().kind, h::Kind::delivery_query);
    f.deliver(f.response());
    REQUIRE_EQ(f.requests.back().kind, h::Kind::fallback_request);
    REQUIRE_EQ(f.requests.back().next_position, 1240U);
    auto denied = f.response();
    denied.outcome = h::Outcome::budget;
    f.deliver(denied);
    REQUIRE_EQ(f.receiver->snapshot().phase, h::ReceiverBridge::Phase::denied);
    REQUIRE_EQ(f.receiver->snapshot().outcome, h::Outcome::budget);
    REQUIRE_EQ(f.receiver->snapshot().fallback,
        h::ReceiverBridge::RequestState::denied);
    f.advance(200);
    REQUIRE_EQ(f.requests.back().kind, h::Kind::delivery_query);
}
TEST(hybrid_receiver_eagain_requires_actual_submission_before_confirmation)
{
    ReceiverFixture f;
    f.p.ct->would_block = 1;
    f.step();
    f.deliver(f.response());
    REQUIRE(!f.receiver->snapshot().fresh);
    f.advance(10);
    f.deliver(f.response());
    REQUIRE(f.receiver->snapshot().fresh);
    REQUIRE_EQ(f.receiver->snapshot().attempts, 2U);
}
TEST(
    hybrid_receiver_claims_application_queue_and_rejects_wrong_role_or_capability)
{
    ReceiverFixture f;
    REQUIRE(
        !h::ReceiverBridge::create(f.p.l, ReceiverFixture::configuration()));
    REQUIRE(
        !h::ReceiverBridge::create(f.p.c, ReceiverFixture::configuration()));
    REQUIRE_EQ(f.p.c->send(Pair::request(), 1000),
        h::NativeService::Admission::invalid);
    h::NativeService::Event e;
    REQUIRE(!f.p.c->pop(e));
    auto bad = ReceiverFixture::configuration();
    bad.maximum_rounds = 9;
    REQUIRE(!h::ReceiverBridge::create(f.p.c, bad));
    Pair legacy;
    auto c = legacy.config(true), l = legacy.config(false);
    c.exchange.policy.supported = l.exchange.policy.supported =
        h::legacy_features;
    legacy.c = h::NativeService::create_for_testing(c);
    legacy.l = h::NativeService::create_for_testing(l);
    legacy.connect();
    REQUIRE(
        !h::ReceiverBridge::create(legacy.c, ReceiverFixture::configuration()));
}
TEST(
    hybrid_receiver_close_grant_revocation_and_expiry_fence_pending_observations)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        ReceiverFixture f;
        f.step();
        if (mode == 0)
            f.receiver->close();
        else if (mode == 1)
            ++f.p.cp->generation;
        else
            f.p.clock->value = 10000;
        f.receiver->pump();
        REQUIRE(f.receiver->snapshot().closed);
        REQUIRE(!f.receiver->snapshot().fresh);
        REQUIRE_EQ(f.receiver->request_fallback(1234),
            h::ReceiverBridge::Admission::closed);
    }
}
TEST(hybrid_receiver_forged_echo_does_not_poison_the_authenticated_reply)
{
    ReceiverFixture f;
    f.step();
    const auto message = f.response();
    REQUIRE_EQ(
        f.p.l->send(message, 1000), h::NativeService::Admission::accepted);
    f.p.l->pump_for_testing();
    const auto wires = f.p.lt->take();
    bool tested = false;
    for (const auto& original : wires) {
        const auto packet = decode_packet(original);
        if (!packet || original.size() != 172)
            continue;
        auto forged = original;
        forged[116] ^= std::byte {1};
        const auto changed = decode_packet(forged);
        REQUIRE(changed);
        REQUIRE(f.p.c->enqueue(changed.packet, f.p.clock->value.load()));
        f.p.c->pump_for_testing();
        f.receiver->pump();
        REQUIRE(!f.receiver->snapshot().observed);
        REQUIRE(f.p.c->enqueue(packet.packet, f.p.clock->value.load()));
        f.p.c->pump_for_testing();
        f.receiver->pump();
        REQUIRE(f.receiver->snapshot().fresh);
        tested = true;
    }
    REQUIRE(tested);
}
TEST(
    hybrid_receiver_full_native_queue_and_duplicate_reports_do_not_renew_timers)
{
    ReceiverFixture f;
    const auto first = decode_packet(f.p.lt->history.front());
    REQUIRE(first);
    for (std::size_t i = 0; i < h::NativeService::capacity; ++i)
        REQUIRE(f.p.c->enqueue(first.packet, f.p.clock->value.load()));
    f.receiver->pump();
    REQUIRE_EQ(f.receiver->snapshot().refused, 1U);
    REQUIRE_EQ(f.receiver->snapshot().attempts, 0U);
    for (unsigned i = 0; i < 16; ++i)
        f.receiver->pump();
    REQUIRE_EQ(f.receiver->snapshot().refused, 1U);
    f.p.c->pump_for_testing();
    f.p.c->pump_for_testing();
    f.advance(10);
    auto message = f.response();
    f.deliver(message);
    const auto before = f.receiver->snapshot();
    REQUIRE(before.fresh);
    f.advance(10);
    f.deliver(message);
    const auto after = f.receiver->snapshot();
    REQUIRE_EQ(after.confirmation_origin_us, before.confirmation_origin_us);
    REQUIRE_EQ(after.confirmed, before.confirmed);
    REQUIRE(after.duplicates > 0);
}
TEST(
    hybrid_receiver_network_and_posting_delay_count_against_freshness_and_lifetime)
{
    auto c = ReceiverFixture::configuration();
    c.round_timeout_us = 200;
    c.confirmation_lifetime_us = 100;
    ReceiverFixture f(c);
    f.step();
    const auto origin = f.p.clock->value.load();
    f.advance(70);
    f.deliver(f.response());
    REQUIRE(f.receiver->snapshot().fresh);
    REQUIRE_EQ(f.receiver->snapshot().confirmation_origin_us, origin);
    f.advance(31);
    REQUIRE(!f.receiver->snapshot().fresh);
    ReceiverFixture posted;
    REQUIRE_EQ(posted.receiver->request_fallback(1234),
        h::ReceiverBridge::Admission::accepted);
    posted.advance(400);
    REQUIRE_EQ(posted.receiver->snapshot().fallback,
        h::ReceiverBridge::RequestState::unknown);
    REQUIRE_EQ(posted.receiver->snapshot().attempts, 0U);
    REQUIRE(posted.requests.empty());
}
#endif

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
namespace {
struct RejoinFixture : MembershipFixture {
    RejoinFixture(unsigned cycles = 1)
    {
        provider->succeed = true;
        auto c = policy();
        c.maximum_recoveries = cycles;
        attach(c);
        membership->pump();
        REQUIRE(membership->snapshot().joined);
    }
    void uc()
    {
        REQUIRE_EQ(receiver->request_fallback(1234),
            h::ReceiverBridge::Admission::accepted);
        step();
        deliver(response(h::Kind::delivery_commit, 1, h::Mode::unicast));
        REQUIRE(receiver->snapshot().fresh);
        REQUIRE_EQ(receiver->snapshot().mode, h::Mode::unicast);
    }
};
}
TEST(hybrid_rejoin_requires_explicit_budget_and_same_receiver)
{
    MembershipFixture f;
    auto c = f.policy();
    c.maximum_recoveries = 9;
    REQUIRE(!h::MembershipRecovery::valid_configuration(c));
    c.maximum_recoveries = 1;
    REQUIRE(!h::MembershipRecovery::create(
        f.p.c, c, {f.provider, MembershipProvider::prepare}));
    ReceiverFixture other;
    REQUIRE(!h::MembershipRecovery::create(
        f.p.c, c, {f.provider, MembershipProvider::prepare}, other.receiver));
    f.provider->succeed = true;
    f.attach();
    f.membership->pump();
    REQUIRE(f.membership->snapshot().joined);
    REQUIRE(!f.membership->report_loss());
}
TEST(
    hybrid_rejoin_loss_waits_for_approved_uc_and_preserves_canonical_observations)
{
    RejoinFixture f;
    const auto first = f.membership->snapshot().route_epoch;
    const auto old_token = compat::HybridAutomaticTestAccess::token(*f.runtime);
    compat::HybridAutomaticTestAccess::multicast(
        f.runtime, f.p.c, 42, f.p.clock->value.load());
    const auto before = f.receiver->data_progress();
    REQUIRE(before);
    REQUIRE(f.membership->report_loss());
    REQUIRE(!f.membership->report_loss());
    REQUIRE(!f.membership->snapshot().joined);
    REQUIRE(f.receiver->data_progress());
    REQUIRE_EQ(
        f.receiver->data_progress()->unique_packets, before->unique_packets);
    f.membership->pump();
    REQUIRE_EQ(f.provider->calls, 1U);
    REQUIRE_EQ(f.membership->snapshot().phase,
        h::MembershipRecovery::Phase::awaiting_unicast);
    auto rogue = std::make_shared<compat::DatagramChannel>();
    REQUIRE(!rogue->register_ingress_route(f.p.c->origin()->dispatch().group_id,
        f.runtime, compat::PacketIngress::multicast_data,
        compat::MulticastIngressConfiguration {f.p.c->origin()->source()}));
    f.uc();
    f.membership->pump();
    REQUIRE(f.membership->snapshot().joined);
    REQUIRE(f.membership->snapshot().route_epoch > first);
    REQUIRE_EQ(f.membership->snapshot().recoveries, 1U);
    REQUIRE_EQ(f.receiver->data_progress()->all.next, before->all.next);
    compat::HybridAutomaticTestAccess::multicast(
        f.runtime, f.p.c, 43, f.p.clock->value.load(), true, old_token);
    REQUIRE_EQ(f.receiver->data_progress()->all.next, before->all.next);
    REQUIRE_EQ(f.provider->calls, 2U);
}
TEST(hybrid_rejoin_cycle_attempts_backoff_and_lifetime_loss_budget_are_bounded)
{
    RejoinFixture f;
    f.uc();
    REQUIRE(f.membership->report_loss());
    f.provider->succeed = false;
    f.membership->pump();
    REQUIRE_EQ(f.membership->snapshot().cycle_attempts, 1U);
    f.advance(10);
    f.membership->pump();
    REQUIRE_EQ(f.provider->calls, 3U);
    f.provider->succeed = true;
    f.advance(20);
    f.membership->pump();
    REQUIRE(f.membership->snapshot().joined);
    REQUIRE_EQ(f.membership->snapshot().attempts, 4U);
    REQUIRE(f.membership->report_loss());
    f.membership->pump();
    REQUIRE_EQ(f.membership->snapshot().phase,
        h::MembershipRecovery::Phase::exhausted);
    REQUIRE(f.membership->snapshot().budget_exhausted);
    for (unsigned i = 0; i < 20; ++i)
        f.membership->pump();
    REQUIRE_EQ(f.provider->calls, 4U);
    REQUIRE(f.receiver->data_progress());
    REQUIRE(!h::MembershipRecovery::create(f.p.c, f.policy()));
}
TEST(
    hybrid_rejoin_deadline_starts_at_loss_and_provider_cannot_refresh_uc_or_time)
{
    for (bool provider_delay : {false, true}) {
        RejoinFixture f;
        f.uc();
        REQUIRE(f.membership->report_loss());
        const auto deadline = f.membership->snapshot().not_after_us;
        if (provider_delay)
            f.provider->effect = [&](const auto& r) {
                REQUIRE_EQ(r.recovery, 1U);
                REQUIRE_EQ(r.attempt, 1U);
                f.p.clock->value = deadline;
            };
        else
            f.p.clock->value = deadline;
        f.membership->pump();
        REQUIRE_EQ(f.membership->snapshot().phase,
            h::MembershipRecovery::Phase::expired);
        REQUIRE(!f.membership->snapshot().joined);
        REQUIRE_EQ(f.membership->snapshot().not_after_us, deadline);
        REQUIRE_EQ(f.provider->calls, provider_delay ? 2U : 1U);
    }
    RejoinFixture stale;
    stale.uc();
    REQUIRE(stale.membership->report_loss());
    stale.provider->effect = [&](const auto&) {
        stale.p.clock->value += 150;
    };
    stale.membership->pump();
    REQUIRE(!stale.membership->snapshot().joined);
    REQUIRE_EQ(stale.membership->snapshot().phase,
        h::MembershipRecovery::Phase::awaiting_unicast);
}
TEST(
    hybrid_rejoin_native_receive_error_preserves_uc_but_unowned_revocation_is_terminal)
{
    for (bool fatal : {false, true}) {
        RejoinFixture f;
        f.uc();
        auto channel = compat::HybridAutomaticTestAccess::membership_channel(
            *f.membership);
        if (fatal) {
            auto receive = [](std::span<std::byte>) noexcept {
                return UdpIoResult {
                    .error = Error::io_error, .system_error = EBADF};
            };
            (void)channel->run_once_for_testing(receive);
            REQUIRE(!f.runtime->broken());
            REQUIRE(f.receiver->data_progress());
            f.membership->pump();
            REQUIRE(!channel->running());
            REQUIRE(f.membership->snapshot().joined);
            REQUIRE_EQ(f.membership->snapshot().recoveries, 1U);
        } else {
            channel->unregister_connection(
                f.p.c->origin()->dispatch().group_id);
            REQUIRE(f.membership->snapshot().closed);
            f.membership->pump();
            REQUIRE_EQ(f.provider->calls, 1U);
            REQUIRE(!f.receiver->data_progress());
        }
    }
}
TEST(hybrid_rejoin_close_during_preparation_discards_late_channel)
{
    RejoinFixture f;
    f.uc();
    REQUIRE(f.membership->report_loss());
    std::promise<void> entered, release;
    auto ready = release.get_future().share();
    f.provider->effect = [&](const auto&) {
        entered.set_value();
        ready.wait();
    };
    auto task = std::async(std::launch::async, [&] {
        f.membership->pump();
    });
    entered.get_future().wait();
    f.membership->close();
    release.set_value();
    task.get();
    REQUIRE(f.membership->snapshot().closed);
    REQUIRE(!f.membership->snapshot().joined);
    REQUIRE_EQ(f.provider->calls, 2U);
    REQUIRE(!f.receiver->data_progress());
}
TEST(hybrid_rejoin_old_coverage_and_queued_final_return_cannot_cross_loss_epoch)
{
    for (unsigned stage = 0; stage < 4; ++stage) {
        RejoinFixture f;
        f.uc();
        REQUIRE_EQ(f.receiver->request_overlap(),
            h::ReceiverBridge::Admission::accepted);
        f.step();
        f.deliver(f.response(h::Kind::delivery_commit, 2, h::Mode::overlap));
        auto at = f.p.clock->value.load();
        compat::HybridAutomaticTestAccess::multicast(f.runtime, f.p.c, 42, at);
        f.p.clock->value += 10;
        compat::HybridAutomaticTestAccess::multicast(
            f.runtime, f.p.c, 43, at + 10);
        const auto proof = f.receiver->coverage_proof(1236, 100);
        REQUIRE(proof);
        REQUIRE(proof->route_epoch);
        if (stage)
            REQUIRE_EQ(f.receiver->request_multicast(*proof),
                h::ReceiverBridge::Admission::accepted);
        if (stage >= 2)
            f.receiver->pump(); // Handed to native queue, not transmitted yet.
        if (stage == 3) {
            unsigned checks = 0;
            bool lost = false;
            f.p.ct->bound_effect = [&] {
                if (++checks == 3)
                    lost = f.membership->report_loss();
            };
            f.p.c->pump_for_testing();
            f.p.ct->bound_effect = {};
            REQUIRE(lost);
        } else
            REQUIRE(f.membership->report_loss());
        REQUIRE(!f.receiver->coverage_proof(1236, 100));
        if (!stage)
            REQUIRE_EQ(f.receiver->request_multicast(*proof),
                h::ReceiverBridge::Admission::invalid);
        f.step();
        if (stage)
            REQUIRE(f.receiver->snapshot().returning
                != h::ReceiverBridge::RequestState::confirmed);
        REQUIRE_EQ(f.receiver->snapshot().mode, h::Mode::overlap);
        for (const auto& m : f.requests)
            REQUIRE(!(m.kind == h::Kind::multicast_ready
                && m.mode == h::Mode::multicast));
    }
}
#endif

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
TEST(hybrid_rejoin_grant_expiry_and_reentrant_loss_cannot_extend_lifetime)
{
    RejoinFixture f;
    f.uc();
    REQUIRE(f.membership->report_loss());
    f.provider->effect = [&](const auto&) {
        f.membership->pump();
        REQUIRE(!f.membership->report_loss());
        f.p.clock->value = f.p.c->snapshot().expires_at_us;
    };
    f.membership->pump();
    REQUIRE(!f.provider->failed);
    REQUIRE(f.membership->snapshot().closed);
    REQUIRE(!f.membership->snapshot().joined);
    REQUIRE(!f.receiver->data_progress());
    REQUIRE_EQ(f.provider->calls, 2U);
}
#endif
