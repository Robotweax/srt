// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/hybrid_native.hpp"
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
    bool alive = true, partial = false, close_send = false;
    unsigned would_block = 0, drop_kind = 0, dropped = 0;
    std::weak_ptr<h::NativeService> service;
    std::mutex mutex;
    std::vector<std::vector<std::byte>> output, history;
    static bool bound(void* p, const h::NativeService*) noexcept
    {
        return static_cast<Transport*>(p)->alive;
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
        x.exchange.policy.supported = h::known_features;
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
    Pair()
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
#endif
