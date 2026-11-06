// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/hybrid_native.hpp"
#include "compat/hybrid_receiver.hpp"
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
namespace {
struct ReceiverFixture {
    Pair p;
    std::shared_ptr<h::ReceiverBridge> receiver;
    std::vector<h::Message> requests;
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
        h::ReceiverBridge::Configuration c = configuration())
    {
        p.connect();
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
