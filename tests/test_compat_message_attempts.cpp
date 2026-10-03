#include "test.hpp"

#include "compat/error_state.hpp"
#include "compat/message_io.hpp"
#include "compat/runtime_scheduler.hpp"
#include "compat/transport_runtime.hpp"
#include "robotweax/srt/codec.hpp"
#include "srt/srt.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

using namespace robotweax::srt;
using namespace robotweax::srt::compat;

namespace {

struct MessageAttemptFixture {
    static constexpr std::size_t payload_size = 800;
    static constexpr std::uint32_t first_sequence = 1000;
    std::vector<std::vector<std::byte>> sent;
    std::shared_ptr<DatagramChannel> channel =
        std::make_shared<DatagramChannel>();
    SocketRecord socket;
    const Ipv4Endpoint peer {.address = {192, 0, 2, 90}, .port = 14900};
    std::uint64_t now = 1000;

    static std::uint64_t clock(void* context) noexcept
    {
        return *static_cast<std::uint64_t*>(context);
    }

    static UdpIoResult capture(
        std::span<const std::byte> bytes, Ipv4Endpoint, void* context) noexcept
    {
        auto& fixture = *static_cast<MessageAttemptFixture*>(context);
        try {
            fixture.sent.emplace_back(bytes.begin(), bytes.end());
            return {.bytes_transferred = bytes.size()};
        } catch (...) {
            return {.error = Error::io_error};
        }
    }

    MessageAttemptFixture()
    {
        channel->set_send_hook_for_testing(capture, this);
        REQUIRE_EQ(socket.native_options.set(
                       SocketOption::maximum_payload_size, payload_size),
            Error::none);
        REQUIRE_EQ(
            socket.native_options.set(SocketOption::send_buffer_packets, 3),
            Error::none);
        REQUIRE_EQ(
            socket.native_options.set(SocketOption::receive_buffer_packets, 4),
            Error::none);
        REQUIRE_EQ(socket.native_options.set(SocketOption::tsbpd_mode, 0),
            Error::none);
        socket.state = SRTS_CONNECTED;
        socket.public_options.maximum_payload_size = payload_size;
        socket.public_options.tsbpd_mode = false;
        socket.public_options.send_synchronous = false;
        socket.public_options.receive_synchronous = false;
        socket.public_options.send_timeout_milliseconds = 0;
        socket.public_options.receive_timeout_milliseconds = 0;
        socket.runtime = std::make_shared<ConnectionRuntime>(
            ConnectionRuntime::Configuration {
                .channel = channel,
                .peer = peer,
                .peer_socket_id = 90,
                .initial_sequence = SequenceNumber {first_sequence},
                .flow_window_packets = 256,
                .options = socket.native_options,
                .origin = ConnectionRuntime::Clock::now(),
                .now_function = clock,
                .now_context = &now,
            });
    }

    void publish_message(std::span<const std::byte> payload)
    {
        REQUIRE_EQ(payload.size(), 2U * payload_size);
        for (std::size_t index = 0; index != 2U; ++index) {
            PacketView packet;
            packet.kind = PacketKind::data;
            packet.data.sequence = SequenceNumber {
                first_sequence + static_cast<std::uint32_t>(index)};
            packet.data.message_number = 7;
            packet.data.in_order = true;
            packet.data.boundary =
                index == 0U ? MessageBoundary::first : MessageBoundary::last;
            packet.payload =
                payload.subspan(index * payload_size, payload_size);
            socket.runtime->process_packet(packet, peer);
        }
    }

    void publish_control(ControlType type)
    {
        const std::array<std::byte, 4> padding {};
        PacketView packet;
        packet.kind = PacketKind::control;
        packet.control.type = type;
        packet.control.type_specific = SRT_EFILE;
        packet.payload = padding;
        socket.runtime->process_packet(packet, peer);
    }
};

} // namespace

TEST(compat_message_attempt_fragmented_send_is_atomic_and_owns_payload)
{
    MessageAttemptFixture fixture;
    std::array<char, 2U * MessageAttemptFixture::payload_size> input;
    input.fill('a');
    SRT_MSGCTRL first = srt_msgctrl_default;
    // A zero configured timeout does not override an immediately available
    // nonblocking commit, including a message larger than one datagram.
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), input.size(), &first),
        static_cast<int>(input.size()));
    REQUIRE_EQ(first.msgno, 1);
    input.fill('b'); // Caller ownership returns at success.
    SRT_MSGCTRL rejected = srt_msgctrl_default;
    rejected.msgno = 77;
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), input.size(), &rejected),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_EASYNCSND);
    REQUIRE_EQ(rejected.msgno, 77);
    REQUIRE_EQ(
        fixture.socket.runtime->buffer_packet_counts().unacknowledged_send, 2U);

    fixture.socket.public_options.send_synchronous = true;
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), input.size(), &rejected),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_ETIMEOUT);
    REQUIRE_EQ(rejected.msgno, 77);
    REQUIRE_EQ(
        fixture.socket.runtime->buffer_packet_counts().unacknowledged_send, 2U);

    SRT_MSGCTRL last = srt_msgctrl_default;
    REQUIRE_EQ(send_message(&fixture.socket, input.data(),
                   MessageAttemptFixture::payload_size, &last),
        static_cast<int>(MessageAttemptFixture::payload_size));
    REQUIRE_EQ(last.msgno, 2); // Rejected attempts consumed no message number.
    for (std::size_t turn = 0; turn != 8U; ++turn) {
        (void)fixture.socket.runtime->poll();
        fixture.now += 1000;
    }
    std::size_t data_count = 0;
    for (const auto& bytes : fixture.sent) {
        const auto decoded = decode_packet(bytes);
        REQUIRE(decoded);
        if (decoded.packet.kind != PacketKind::data)
            continue;
        REQUIRE_EQ(decoded.packet.data.sequence,
            SequenceNumber {MessageAttemptFixture::first_sequence
                + static_cast<std::uint32_t>(data_count)});
        REQUIRE_EQ(
            decoded.packet.data.message_number, data_count < 2U ? 1U : 2U);
        REQUIRE_EQ(
            decoded.packet.payload.size(), MessageAttemptFixture::payload_size);
        REQUIRE(std::all_of(decoded.packet.payload.begin(),
            decoded.packet.payload.end(), [data_count](std::byte value) {
                return value
                    == (data_count < 2U ? std::byte {'a'} : std::byte {'b'});
            }));
        ++data_count;
    }
    REQUIRE_EQ(data_count, 3U);
}

TEST(compat_message_attempt_receive_timeout_does_not_retain_caller_storage)
{
    MessageAttemptFixture fixture;
    std::array<char, 1601> expired;
    expired.fill('x');
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.msgno = 77;
    control.pktseq = 88;
    control.srctime = 99;
    REQUIRE_EQ(receive_message(
                   &fixture.socket, expired.data(), expired.size(), &control),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_EASYNCRCV);
    fixture.socket.public_options.receive_synchronous = true;
    REQUIRE_EQ(receive_message(
                   &fixture.socket, expired.data(), expired.size(), &control),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_ETIMEOUT);
    REQUIRE_EQ(control.msgno, 77);
    REQUIRE_EQ(control.pktseq, 88);
    REQUIRE_EQ(control.srctime, 99);

    std::array<std::byte, 1600> input;
    input.fill(std::byte {'c'});
    fixture.publish_message(input);
    input.fill(std::byte {'d'});
    REQUIRE(std::all_of(expired.begin(), expired.end(), [](char value) {
        return value == 'x';
    }));

    std::array<char, 1599> small;
    small.fill('y');
    REQUIRE_EQ(
        receive_message(&fixture.socket, small.data(), small.size(), &control),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_ELARGEMSG);
    REQUIRE_EQ(control.msgno, 77);
    REQUIRE_EQ(control.pktseq, 88);
    REQUIRE_EQ(control.srctime, 99);
    REQUIRE(std::all_of(small.begin(), small.end(), [](char value) {
        return value == 'y';
    }));
    REQUIRE_EQ(
        fixture.socket.runtime->buffer_packet_counts().available_receive, 2U);

    std::array<char, 1601> received;
    received.fill('z');
    REQUIRE_EQ(receive_message(
                   &fixture.socket, received.data(), received.size(), &control),
        1600);
    REQUIRE_EQ(control.msgno, 7);
    REQUIRE_EQ(control.pktseq,
        static_cast<int>(MessageAttemptFixture::first_sequence));
    REQUIRE_EQ(received.back(), 'z');
    REQUIRE(
        std::all_of(received.begin(), received.begin() + 1600, [](char value) {
            return value == 'c';
        }));
    REQUIRE_EQ(
        fixture.socket.runtime->buffer_packet_counts().available_receive, 0U);
    REQUIRE(std::all_of(expired.begin(), expired.end(), [](char value) {
        return value == 'x';
    }));
}

TEST(compat_message_attempt_peer_error_precedes_pressure_and_is_consumed_once)
{
    MessageAttemptFixture fixture;
    const std::array<char, 2400> input {};
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), input.size(), nullptr),
        2400);
    fixture.publish_control(ControlType::peer_error);
    fixture.socket.public_options.send_synchronous = true;
    SRT_MSGCTRL control = srt_msgctrl_default;
    control.msgno = 77;
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), 1, &control), SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_EPEERERR);
    REQUIRE_EQ(control.msgno, 77);
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), 1, &control), SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_ETIMEOUT);
    fixture.socket.public_options.send_synchronous = false;
    REQUIRE_EQ(
        send_message(&fixture.socket, input.data(), 1, &control), SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_EASYNCSND);
    REQUIRE_EQ(
        fixture.socket.runtime->buffer_packet_counts().unacknowledged_send, 3U);
}

TEST(compat_message_attempt_peer_shutdown_preserves_message_tail)
{
    MessageAttemptFixture fixture;
    std::array<std::byte, 1600> input;
    input.fill(std::byte {'t'});
    fixture.publish_message(input);
    fixture.publish_control(ControlType::shutdown);
    fixture.socket.state = SRTS_BROKEN;
    fixture.socket.public_options.receive_synchronous = true;
    std::array<char, 1600> output {};
    SRT_MSGCTRL control = srt_msgctrl_default;
    REQUIRE_EQ(receive_message(
                   &fixture.socket, output.data(), output.size(), &control),
        1600);
    REQUIRE_EQ(control.msgno, 7);
    REQUIRE(std::all_of(output.begin(), output.end(), [](char value) {
        return value == 't';
    }));
    REQUIRE_EQ(receive_message(
                   &fixture.socket, output.data(), output.size(), &control),
        SRT_ERROR);
    REQUIRE_EQ(last_error().code, SRT_ECONNLOST);
    REQUIRE_EQ(control.msgno, 7);
}

namespace {

// Freeze a worker before any target-runtime lock is taken. This is a service
// qualification fixture, not a transport-affinity implementation.
struct AdmissionWorkerGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool open = false;

    static void pause(void* context) noexcept
    {
        auto& gate = *static_cast<AdmissionWorkerGate*>(context);
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&gate] {
            return gate.open;
        });
    }

    bool await_entry()
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds {2}, [this] {
            return entered;
        });
    }

    void release() noexcept
    {
        std::lock_guard lock(mutex);
        open = true;
        changed.notify_all();
    }
};

struct AdmissionGateRelease {
    std::shared_ptr<AdmissionWorkerGate> gate;
    ~AdmissionGateRelease()
    {
        gate->release();
    }
};

struct OwnedSendProbe {
    std::shared_ptr<MessageAttemptFixture> fixture;
    std::array<char, 1600> input {};
    std::promise<int> completion;

    static void commit(void* context) noexcept
    {
        auto& probe = *static_cast<OwnedSendProbe*>(context);
        probe.completion.set_value(send_message(&probe.fixture->socket,
            probe.input.data(), probe.input.size(), nullptr));
    }
};

struct AdapterProbeResult {
    int sent = SRT_ERROR;
    int received = SRT_ERROR;
    int send_message_number = 0;
    int receive_message_number = 0;
    bool on_worker = false;
    bool payload_matches = false;
};

struct OwnedAdapterProbe {
    std::shared_ptr<MessageAttemptFixture> fixture;
    std::array<char, 800> input {};
    std::array<char, 1600> output {};
    std::promise<AdapterProbeResult> completion;

    AdapterProbeResult attempt() noexcept
    {
        SRT_MSGCTRL send_control = srt_msgctrl_default;
        SRT_MSGCTRL receive_control = srt_msgctrl_default;
        AdapterProbeResult result;
        result.on_worker = RuntimeScheduler::on_worker_thread();
        result.sent = send_message(
            &fixture->socket, input.data(), input.size(), &send_control);
        result.received = receive_message(
            &fixture->socket, output.data(), output.size(), &receive_control);
        result.send_message_number = send_control.msgno;
        result.receive_message_number = receive_control.msgno;
        result.payload_matches =
            std::all_of(output.begin(), output.end(), [](char value) {
                return value == 'p';
            });
        return result;
    }

    static void commit(void* context) noexcept
    {
        auto& probe = *static_cast<OwnedAdapterProbe*>(context);
        probe.completion.set_value(probe.attempt());
    }
};

} // namespace

TEST(compat_message_attempt_paused_service_does_not_gate_application_commit)
{
    auto fixture = std::make_shared<MessageAttemptFixture>();
    std::array<std::byte, 1600> received_payload;
    received_payload.fill(std::byte {'p'});
    fixture->publish_message(received_payload);
    auto gate = std::make_shared<AdmissionWorkerGate>();
    auto send_probe = std::make_shared<OwnedSendProbe>();
    send_probe->fixture = fixture;
    auto service_result = send_probe->completion.get_future();
    auto adapter_probe = std::make_shared<OwnedAdapterProbe>();
    adapter_probe->fixture = fixture;
    RuntimeScheduler scheduler {{.shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 1,
        .service_capacity_per_shard = 1}};
    // The gate releases before this async future's destructor on every failure.
    std::future<AdapterProbeResult> application_result;
    AdmissionGateRelease release {gate};
    REQUIRE(scheduler.start());
    REQUIRE_EQ(scheduler.submit(0, {AdmissionWorkerGate::pause, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE(gate->await_entry());
    const auto reserved =
        scheduler.reserve_service(0, {OwnedSendProbe::commit, send_probe});
    REQUIRE_EQ(reserved.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(
        scheduler.reserve_service(0, {OwnedSendProbe::commit, send_probe})
            .status,
        RuntimeScheduler::SubmitStatus::full);
    REQUIRE_EQ(scheduler.notify_service(reserved.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.snapshot().services_pending, 1U);
    REQUIRE_EQ(service_result.wait_for(std::chrono::seconds {0}),
        std::future_status::timeout);

    application_result = std::async(std::launch::async, [adapter_probe] {
        return adapter_probe->attempt();
    });
    REQUIRE_EQ(application_result.wait_for(std::chrono::seconds {2}),
        std::future_status::ready);
    const auto result = application_result.get();
    REQUIRE_EQ(result.sent, 800);
    REQUIRE_EQ(result.received, 1600);
    REQUIRE_EQ(result.send_message_number, 1);
    REQUIRE_EQ(result.receive_message_number, 7);
    REQUIRE(!result.on_worker);
    REQUIRE(result.payload_matches);
    // Guaranteed admission is not synchronous commit on a frozen worker.
    REQUIRE_EQ(service_result.wait_for(std::chrono::seconds {0}),
        std::future_status::timeout);
    REQUIRE_EQ(
        fixture->socket.runtime->buffer_packet_counts().unacknowledged_send,
        1U);
    REQUIRE_EQ(
        fixture->socket.runtime->buffer_packet_counts().available_receive, 0U);

    gate->release();
    REQUIRE_EQ(service_result.wait_for(std::chrono::seconds {2}),
        std::future_status::ready);
    REQUIRE_EQ(service_result.get(), 1600);
    REQUIRE_EQ(
        fixture->socket.runtime->buffer_packet_counts().unacknowledged_send,
        3U);
    REQUIRE(scheduler.release_service(reserved.token));
    scheduler.stop();
}

TEST(compat_message_attempt_cross_shard_nonblocking_call_needs_no_service_rpc)
{
    auto fixture = std::make_shared<MessageAttemptFixture>();
    std::array<std::byte, 1600> received_payload;
    received_payload.fill(std::byte {'p'});
    fixture->publish_message(received_payload);
    auto gate = std::make_shared<AdmissionWorkerGate>();
    auto send_probe = std::make_shared<OwnedSendProbe>();
    send_probe->fixture = fixture;
    auto service_result = send_probe->completion.get_future();
    auto adapter_probe = std::make_shared<OwnedAdapterProbe>();
    adapter_probe->fixture = fixture;
    auto application_result = adapter_probe->completion.get_future();
    RuntimeScheduler scheduler {{.shard_count = 2,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 1,
        .service_capacity_per_shard = 1}};
    AdmissionGateRelease release {gate};
    REQUIRE(scheduler.start());
    REQUIRE_EQ(scheduler.submit(1, {AdmissionWorkerGate::pause, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE(gate->await_entry());
    const auto target =
        scheduler.reserve_service(1, {OwnedSendProbe::commit, send_probe});
    REQUIRE_EQ(target.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(target.token),
        RuntimeScheduler::SubmitStatus::accepted);
    const auto caller = scheduler.reserve_service(
        0, {OwnedAdapterProbe::commit, adapter_probe});
    REQUIRE_EQ(caller.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(caller.token),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(application_result.wait_for(std::chrono::seconds {2}),
        std::future_status::ready);
    const auto result = application_result.get();
    REQUIRE(result.on_worker);
    REQUIRE_EQ(result.sent, 800);
    REQUIRE_EQ(result.received, 1600);
    REQUIRE_EQ(result.send_message_number, 1);
    REQUIRE_EQ(result.receive_message_number, 7);
    REQUIRE(result.payload_matches);
    REQUIRE_EQ(service_result.wait_for(std::chrono::seconds {0}),
        std::future_status::timeout);
    REQUIRE_EQ(scheduler.snapshot().services_pending, 1U);
    gate->release();
    REQUIRE_EQ(service_result.wait_for(std::chrono::seconds {2}),
        std::future_status::ready);
    REQUIRE_EQ(service_result.get(), 1600);
    REQUIRE(scheduler.release_service(caller.token));
    REQUIRE(scheduler.release_service(target.token));
    scheduler.stop();
}

TEST(compat_message_attempt_many_callers_report_only_real_buffer_pressure)
{
    auto fixture = std::make_shared<MessageAttemptFixture>();
    auto worker_gate = std::make_shared<AdmissionWorkerGate>();
    auto callers_gate = std::make_shared<AdmissionWorkerGate>();
    auto send_probe = std::make_shared<OwnedSendProbe>();
    send_probe->fixture = fixture;
    auto service_result = send_probe->completion.get_future();
    struct Outcome {
        int sent;
        int message_number;
        SRT_ERRNO error;
    };
    RuntimeScheduler scheduler {{.shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 1,
        .service_capacity_per_shard = 1}};
    std::vector<std::future<Outcome>> callers;
    callers.reserve(16);
    AdmissionGateRelease release_worker {worker_gate};
    AdmissionGateRelease release_callers {callers_gate};
    REQUIRE(scheduler.start());
    REQUIRE_EQ(scheduler.submit(0, {AdmissionWorkerGate::pause, worker_gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE(worker_gate->await_entry());
    const auto target =
        scheduler.reserve_service(0, {OwnedSendProbe::commit, send_probe});
    REQUIRE_EQ(target.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(target.token),
        RuntimeScheduler::SubmitStatus::accepted);
    for (std::size_t index = 0; index != 16U; ++index) {
        callers.push_back(
            std::async(std::launch::async, [fixture, callers_gate] {
                AdmissionWorkerGate::pause(callers_gate.get());
                const std::array<char, 800> input {};
                SRT_MSGCTRL control = srt_msgctrl_default;
                control.msgno = 99;
                const int sent = send_message(
                    &fixture->socket, input.data(), input.size(), &control);
                return Outcome {sent, control.msgno, last_error().code};
            }));
    }
    callers_gate->release();
    std::array<bool, 4> committed {};
    std::size_t successes = 0;
    for (auto& caller : callers) {
        REQUIRE_EQ(caller.wait_for(std::chrono::seconds {2}),
            std::future_status::ready);
        const auto result = caller.get();
        if (result.sent == 800) {
            REQUIRE(result.message_number > 0 && result.message_number < 4);
            REQUIRE(
                !committed[static_cast<std::size_t>(result.message_number)]);
            committed[static_cast<std::size_t>(result.message_number)] = true;
            ++successes;
        } else {
            REQUIRE_EQ(result.sent, SRT_ERROR);
            REQUIRE_EQ(result.error, SRT_EASYNCSND);
            REQUIRE_EQ(result.message_number, 99);
        }
    }
    REQUIRE_EQ(successes, 3U);
    REQUIRE_EQ(
        fixture->socket.runtime->buffer_packet_counts().unacknowledged_send,
        3U);
    REQUIRE_EQ(service_result.wait_for(std::chrono::seconds {0}),
        std::future_status::timeout);
    worker_gate->release();
    REQUIRE_EQ(service_result.wait_for(std::chrono::seconds {2}),
        std::future_status::ready);
    REQUIRE_EQ(service_result.get(), SRT_ERROR);
    REQUIRE(scheduler.release_service(target.token));
    scheduler.stop();
}

TEST(compat_message_attempt_local_close_completes_without_paused_service)
{
    auto fixture = std::make_shared<MessageAttemptFixture>();
    const std::array<char, 2400> initial {};
    REQUIRE_EQ(
        send_message(&fixture->socket, initial.data(), initial.size(), nullptr),
        2400);
    fixture->socket.public_options.send_synchronous = true;
    fixture->socket.public_options.send_timeout_milliseconds = -1;
    auto gate = std::make_shared<AdmissionWorkerGate>();
    auto send_probe = std::make_shared<OwnedSendProbe>();
    send_probe->fixture = fixture;
    auto service_result = send_probe->completion.get_future();
    RuntimeScheduler scheduler {{.shard_count = 1,
        .queue_capacity_per_shard = 1,
        .timer_capacity_per_shard = 1,
        .service_capacity_per_shard = 1}};
    std::future<SRT_ERRNO> application_result;
    AdmissionGateRelease release {gate};
    REQUIRE(scheduler.start());
    REQUIRE_EQ(scheduler.submit(0, {AdmissionWorkerGate::pause, gate}),
        RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE(gate->await_entry());
    const auto target =
        scheduler.reserve_service(0, {OwnedSendProbe::commit, send_probe});
    REQUIRE_EQ(target.status, RuntimeScheduler::SubmitStatus::accepted);
    REQUIRE_EQ(scheduler.notify_service(target.token),
        RuntimeScheduler::SubmitStatus::accepted);
    application_result = std::async(std::launch::async, [fixture] {
        const char input = 'c';
        if (send_message(&fixture->socket, &input, 1, nullptr) != SRT_ERROR)
            return SRT_SUCCESS;
        return last_error().code;
    });
    // Either ordering of the application's entry and local close must finish;
    // the test does not assume the application has already reached its CV wait.
    fixture->socket.runtime->close();
    REQUIRE_EQ(application_result.wait_for(std::chrono::seconds {2}),
        std::future_status::ready);
    REQUIRE_EQ(application_result.get(), SRT_ESCLOSED);
    REQUIRE_EQ(service_result.wait_for(std::chrono::seconds {0}),
        std::future_status::timeout);
    gate->release();
    REQUIRE_EQ(service_result.wait_for(std::chrono::seconds {2}),
        std::future_status::ready);
    REQUIRE_EQ(service_result.get(), SRT_ERROR);
    REQUIRE(scheduler.release_service(target.token));
    scheduler.stop();
}
