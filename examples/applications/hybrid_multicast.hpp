/* SPDX-License-Identifier: MIT */
#pragma once
#include "common.hpp"
#if defined(ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL)                         \
    && defined(ROBOTWEAX_SAMPLE_HYBRID)
#include "compat/group_registry.hpp"
#include "compat/group_send_coordinator.hpp"
#include "compat/socket_registry.hpp"
#include "compat/hybrid/hybrid_setup.hpp"
#include "compat/hybrid/hybrid_automatic.hpp"
#include "compat/hybrid/hybrid_automatic_return.hpp"
#include <span>

namespace samples {
namespace hybrid = robotweax::srt::compat::hybrid;
using robotweax::srt::compat::GroupRegistry;
using robotweax::srt::compat::SocketRegistry;
// This sample is an explicit host for the internal experimental API. It is
// compiled only with hybrid enabled; no public SRT option is invented here.
class HybridMedia {
    struct Credentials {
        std::map<std::string, std::array<std::byte, 32>> keys;
        std::uint64_t expires = 0;
        bool caller = false;
        std::mutex mutex;
        hybrid::Context context;
        std::uint64_t generation = 0, next = 0, discarded = 0;
        std::weak_ptr<robotweax::srt::compat::ConnectionRuntime> runtime;
        std::weak_ptr<hybrid::ReceiverBridge> receiver;
        robotweax::srt::SequenceNumber sequence;
        ~Credentials()
        {
            for (auto& entry : keys)
                robotweax::srt::default_crypto_provider().secure_erase(
                    entry.second);
        }
        static bool resolve(void* p, const hybrid::CredentialRequest& r,
            hybrid::Authorization& a, std::span<std::byte, 32> secret) noexcept
        {
            auto& self = *static_cast<Credentials*>(p);
            const auto name = r.stream_id.view();
            // Credentials are deliberately scoped to the caller's Stream ID.
            // The shared protocol credential label is not a shared secret.
            const auto key = std::find_if(
                self.keys.begin(), self.keys.end(), [&](const auto& entry) {
                    return name == entry.first;
                });
            if (r.caller != self.caller || key == self.keys.end()
                || hybrid::NativeService::monotonic_now() >= self.expires)
                return false;
            a = {.credential_id = r.credential_id,
                .caller_group = r.caller_group,
                .listener_group = r.listener_group,
                .stream_id = r.stream_id,
                .generation = 1,
                .expires_at_us = self.expires};
            std::copy(key->second.begin(), key->second.end(), secret.begin());
            return true;
        }
        static bool revalidate(void* p, const hybrid::CredentialRequest& r,
            const hybrid::Authorization& old,
            hybrid::Authorization& current) noexcept
        {
            auto& self = *static_cast<Credentials*>(p);
            current = old;
            return old.expires_at_us == self.expires
                && hybrid::NativeService::monotonic_now() < self.expires
                && std::any_of(
                    self.keys.begin(), self.keys.end(),
                    [&](const auto& entry) {
                        return r.stream_id.view() == entry.first;
                    });
        }
        static bool enroll(void* p, const hybrid::Context& c,
            const hybrid::Authorization&, std::uint64_t generation) noexcept
        {
            auto& self = *static_cast<Credentials*>(p);
            if (self.caller) {
                std::lock_guard lock(self.mutex);
                if (self.generation)
                    return false;
                self.context = c;
                self.generation = generation;
                self.next = c.selection.initial_position;
                self.sequence = robotweax::srt::SequenceNumber {
                    c.dispatch.initial_sequence};
            }
            return true;
        }
        static std::optional<hybrid::AutomaticCursor> cursor(
            void* p, const hybrid::AutomaticRequest& request) noexcept
        {
            auto& self = *static_cast<Credentials*>(p);
            std::lock_guard lock(self.mutex);
            if (request.generation != self.generation
                || request.stream != self.context.selection.stream
                || request.member != self.context.selection.member)
                return {};
            const auto runtime = self.runtime.lock();
            const auto receiver = self.receiver.lock();
            const auto data =
                receiver ? receiver->data_progress() : std::nullopt;
            if (!runtime || !data || !data->valid)
                return {};
            // Live catch-up policy: explicitly discard unread media through
            // the last locally validated DATA position. That local mutation,
            // not a peer report/high-water claim, authorizes the merge cursor.
            // Otherwise already ACKed, unread TSBPD data may be outside the
            // sender's retained history and make a fallback request impossible.
            const auto next = std::max(self.next, data->all.next);
            const auto skip = next - self.next;
            if (skip >= robotweax::srt::SequenceNumber::half_range)
                return {};
            const auto sequence =
                self.sequence.advanced(static_cast<std::uint32_t>(skip));
            if (!runtime->discard_received_before(sequence))
                return {};
            self.next = next;
            self.sequence = sequence;
            self.discarded += skip;
            return hybrid::AutomaticCursor {self.next,
                hybrid::NativeService::monotonic_now(), self.generation};
        }
    };
    std::shared_ptr<Credentials> credentials_;
    std::shared_ptr<hybrid::StartupSession> startup_;
    std::shared_ptr<hybrid::ReceiverBridge> receiver_;
    std::shared_ptr<hybrid::AutomaticFallback> fallback_;
    std::shared_ptr<hybrid::AutomaticReturn> returning_;
    std::shared_ptr<robotweax::srt::compat::GroupSendCoordinator> source_;
    std::optional<hybrid::Mode> reported_;
    int cycles_ = 4;
    std::uint64_t reported_discarded_ = 0;
#ifdef ROBOTWEAX_SAMPLE_TESTING
    std::shared_ptr<robotweax::srt::compat::DatagramChannel> test_channel_;
    std::uint64_t first_data_ = 0;
    std::uint64_t outage_start_ = 0, outage_end_ = 0;
    static robotweax::srt::UdpIoResult fault_send(
        std::span<const std::byte> bytes, robotweax::srt::IpEndpoint peer,
        void* p) noexcept
    {
        auto& self = *static_cast<HybridMedia*>(p);
        const auto now = hybrid::NativeService::monotonic_now();
        if (!self.first_data_)
            self.first_data_ = now;
        const auto elapsed = now - self.first_data_;
        if (elapsed >= self.outage_start_ && elapsed < self.outage_end_)
            return {.bytes_transferred = bytes.size()};
        return self.test_channel_->socket.send_to(bytes, peer);
    }
#endif
public:
    explicit HybridMedia(const Arguments& args)
    {
        const bool enabled = args.values.contains("--hybrid");
        for (const auto& entry : args.values)
            if (entry.first.starts_with("--hybrid-") && !enabled)
                throw std::runtime_error("hybrid options require --hybrid");
        if (!enabled)
            return;
        credentials_ = std::make_shared<Credentials>();
        credentials_->caller = args.role == "receive";
        const auto env = args.get("--hybrid-secret-env");
        const char* value = env.empty() ? nullptr : std::getenv(env.c_str());
        if (!value || !*value)
            throw std::runtime_error("--hybrid-secret-env must name a nonempty "
                                     "credential environment variable");
        // Parse views: never include credential contents in error messages.
        std::string_view remaining(value);
        while (!remaining.empty()) {
            const auto separator = remaining.find(';');
            const auto entry = remaining.substr(0, separator);
            const auto equal = entry.find('=');
            if (equal == 0 || equal == std::string_view::npos || equal > 64
                || entry.size() - equal - 1 != 64
                || credentials_->keys.size() >= 16)
                throw std::runtime_error(
                    "hybrid credentials must be id=64-hex-digit-secret entries "
                    "separated by semicolons");
            const auto id = entry.substr(0, equal);
            for (char c : id)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '-' || c == '_'))
                    throw std::runtime_error(
                        "hybrid receiver IDs must be alphanumeric, dash or "
                        "underscore");
            std::array<std::byte, 32> key {};
            for (std::size_t i = 0; i < key.size(); ++i) {
                unsigned byte = 0;
                const auto begin = entry.data() + equal + 1 + i * 2;
                const auto parsed = std::from_chars(begin, begin + 2, byte, 16);
                if (parsed.ec != std::errc {} || parsed.ptr != begin + 2)
                    throw std::runtime_error(
                        "hybrid secret must contain only hexadecimal digits");
                key[i] = static_cast<std::byte>(byte);
            }
            if (std::all_of(key.begin(), key.end(),
                    [](auto byte) {
                        return byte == std::byte {};
                    })
                || std::any_of(credentials_->keys.begin(),
                    credentials_->keys.end(),
                    [&](const auto& old) {
                        return old.second == key;
                    })
                || !credentials_->keys.emplace(std::string(id), key).second)
                throw std::runtime_error("hybrid credentials require distinct "
                                         "IDs and nonzero pairwise secrets");
            robotweax::srt::default_crypto_provider().secure_erase(key);
            if (separator == std::string_view::npos)
                break;
            remaining.remove_prefix(separator + 1);
            if (remaining.empty())
                throw std::runtime_error("empty hybrid credential entry");
        }
        const auto id = args.get("--hybrid-id");
        if (credentials_->caller
            && (credentials_->keys.size() != 1
                || !credentials_->keys.contains(id)))
            throw std::runtime_error(
                "receiver requires --hybrid-id and exactly its own credential");
        if (credentials_->caller && args.values.contains("--hybrid-maxbw"))
            throw std::runtime_error("--hybrid-maxbw is sender-only");
        if (!credentials_->caller && args.values.contains("--hybrid-cycles"))
            throw std::runtime_error("--hybrid-cycles is receiver-only");
        if (!credentials_->caller && !id.empty())
            throw std::runtime_error("--hybrid-id is receiver-only");
        if (args.integer("--repeat-threshold", 0, 0, 16) != 0)
            throw std::runtime_error(
                "hybrid coordinated mode requires --repeat-threshold 0");
        (void)args.integer("--hybrid-maxbw", 1'000'000, 1, 1'000'000'000);
        cycles_ = args.integer("--hybrid-cycles", 4, 1, 8);
        credentials_->expires = hybrid::NativeService::monotonic_now()
            + std::uint64_t(args.integer("--duration", 20, 3, 3600)
                  + args.integer("--timeout-ms", 10000, 1, 120000) / 1000 + 30)
                * 1'000'000;
    }
    bool enabled() const
    {
        return bool(credentials_);
    }
    void provision(
        SRTSOCKET id, const Arguments& args, const sockaddr_in& interface)
    {
        if (!enabled())
            return;
        option(id, SRTO_ROBOTWEAX_MCAST_INDEPENDENT, true);
        if (!credentials_->caller) {
            option(id, SRTO_ROBOTWEAX_MCAST_COORDINATED, true);
            auto source = interface;
            source.sin_port = 0;
            option(id, SRTO_ROBOTWEAX_MCAST_SOURCE, source);
            option(id, SRTO_MAXBW,
                std::int64_t(args.integer(
                    "--hybrid-maxbw", 1'000'000, 1, 1'000'000'000)));
        } else {
            text_option(id, SRTO_STREAMID, args.get("--hybrid-id"));
        }
        hybrid::StartupOptions o;
        o.native.owner = credentials_;
        o.native.resolve = Credentials::resolve;
        o.native.revalidate = Credentials::revalidate;
        o.native.enroll = Credentials::enroll;
        o.native.credential_id.fill(
            std::byte {0x31}); // protocol label, not a secret
        o.native.policy.supported |= hybrid::startup_fallback
            | hybrid::policy_query | hybrid::return_control
            | hybrid::overlap_control;
        o.native.policy.required = o.native.policy.supported;
        o.native.retry_interval_us = 100'000;
        o.native.timeout_us = 2'000'000;
        o.native.maximum_sends_per_stage = 8;
        o.admission_timeout_us = 4'000'000;
        o.poll_interval_us = 5'000;
        const auto ip = ntohl(interface.sin_addr.s_addr);
        o.membership = {
            robotweax::srt::IpEndpoint {robotweax::srt::Ipv4Endpoint {
                .address = {static_cast<std::uint8_t>(ip >> 24),
                    static_cast<std::uint8_t>(ip >> 16),
                    static_cast<std::uint8_t>(ip >> 8),
                    static_cast<std::uint8_t>(ip)},
                .port = 0}},
            8, 3'000'000, 100'000, 500'000, 8};
        o.receiver.reporting = o.source.reporting = {50'000, 1'500'000};
        o.receiver.detection = hybrid::DetectionConfiguration {1'500'000,
            50'000, 300'000, 1'500'000, 500'000, 300'000, 500'000, 250'000, 3};
        o.receiver.retry_interval_us = 50'000;
        o.receiver.round_timeout_us = 200'000;
        o.receiver.operation_timeout_us = 1'500'000;
        o.receiver.query_interval_us = 100'000;
        o.receiver.confirmation_lifetime_us = 600'000;
        o.source.returning =
            hybrid::ReturnPolicy {2'000'000, 256, 4, 50'000, 500'000};
        if (!hybrid::provision_startup(id, std::move(o)))
            throw std::runtime_error(
                "cannot provision hybrid multicast startup");
    }
    void attach(SRTSOCKET group, const Arguments& args)
    {
        if (!enabled())
            return;
        const auto record = GroupRegistry::instance().find(group);
        if (!record)
            throw std::runtime_error("hybrid group missing");
        SRTSOCKET member = SRT_INVALID_SOCK;
        {
            std::lock_guard lock(record->mutex);
            if (!record->members.empty())
                member = record->members.front().public_data.id;
            source_ = record->dispatch->coordinator;
#ifdef ROBOTWEAX_SAMPLE_TESTING
            if (args.values.contains("--test-multicast-outage")
                && !credentials_->caller) {
                test_channel_ = record->dispatch->transmit_channel;
                outage_start_ = 2'000'000;
                outage_end_ = 3'500'000;
                if (!test_channel_)
                    throw std::runtime_error("test multicast source missing");
                test_channel_->set_send_hook_for_testing(fault_send, this);
            }
#endif
        }
        if (!credentials_->caller) {
            if (!source_)
                throw std::runtime_error("hybrid coordinator missing");
            return;
        }
        const auto socket = SocketRegistry::instance().find(member);
        if (socket) {
            std::lock_guard lock(socket->mutex);
            startup_ = socket->hybrid_startup_session;
            credentials_->runtime = socket->runtime;
        }
        receiver_ = startup_ ? startup_->receiver() : nullptr;
        credentials_->receiver = receiver_;
        if (!receiver_)
            throw std::runtime_error("hybrid receiver unavailable");
        fallback_ = hybrid::AutomaticFallback::create(receiver_,
            {static_cast<std::uint32_t>(cycles_), 500'000},
            {credentials_, Credentials::cursor});
        returning_ = hybrid::AutomaticReturn::create(receiver_,
            {static_cast<std::uint32_t>(cycles_), 1'500'000, 8, 50'000,
                500'000});
        if (!fallback_ || !returning_)
            throw std::runtime_error(
                "cannot attach automatic hybrid controllers");
        tick();
    }
    void delivered(const SRT_MSGCTRL& control)
    {
        if (!enabled())
            return;
        if (source_) {
            if (!source_->declare_source_activity(hybrid::Activity::producing,
                    source_->snapshot().accepted_frames, 1'500'000))
                throw std::runtime_error("hybrid source activity rejected");
        } else {
            if (control.pktseq < 0)
                throw std::runtime_error("hybrid media sequence missing");
            std::lock_guard lock(credentials_->mutex);
            const robotweax::srt::SequenceNumber sequence {
                static_cast<std::uint32_t>(control.pktseq)};
            const auto distance =
                sequence.distance_from(credentials_->sequence);
            if (distance < 0)
                throw std::runtime_error("hybrid media arrived out of order");
            // Each sample UDP datagram is one SRT packet. Live delivery accepts
            // any explicit gap before this consumed message as dropped media.
            credentials_->next += static_cast<std::uint64_t>(distance) + 1;
            credentials_->sequence = sequence.next();
        }
    }
    void report()
    {
        if (!receiver_)
            return;
        const auto a = receiver_->path_assessment();
        const auto data = receiver_->data_progress();
        const auto f = fallback_->snapshot();
        const auto r = returning_->snapshot();
        static constexpr const char* outcomes[] = {"ok", "denied",
            "history_unavailable", "budget", "stale", "busy", "unsupported"};
        event("\"event\":\"hybrid_stats\",\"fallback_operations\":"
            + std::to_string(f.operations) + ",\"fallback_outcome\":\""
            + outcomes[static_cast<unsigned>(f.outcome)] + "\""
            + ",\"fallback_exhausted\":"
            + (f.budget_exhausted ? "true" : "false")
            + ",\"return_operations\":" + std::to_string(r.operations)
            + ",\"return_outcome\":\""
            + outcomes[static_cast<unsigned>(r.outcome)] + "\""
            + ",\"return_exhausted\":" + (r.budget_exhausted ? "true" : "false")
            + ",\"multicast_stalled\":"
            + (a && a->multicast_stalled ? "true" : "false")
            + ",\"received_multicast_packets\":"
            + std::to_string(data ? data->multicast.packets : 0)
            + ",\"received_unicast_packets\":"
            + std::to_string(data ? data->unicast.packets : 0));
    }
    void tick()
    {
        if (!receiver_)
            return;
        fallback_->pump();
        returning_->pump();
        {
            std::lock_guard lock(credentials_->mutex);
            if (credentials_->discarded != reported_discarded_) {
                reported_discarded_ = credentials_->discarded;
                event(
                    "\"event\":\"hybrid_discontinuity\",\"skipped_positions\":"
                    + std::to_string(reported_discarded_));
            }
        }
        const auto state = receiver_->snapshot();
        if (state.closed)
            throw std::runtime_error("hybrid receiver closed");
        if (state.fresh && (!reported_ || *reported_ != state.mode)) {
            reported_ = state.mode;
            const char* mode = state.mode == hybrid::Mode::multicast
                ? "multicast"
                : state.mode == hybrid::Mode::unicast ? "unicast"
                                                      : "overlap";
            event(std::string("\"event\":\"hybrid_path\",\"mode\":\"") + mode
                + "\",\"revision\":" + std::to_string(state.revision));
        }
    }
    ~HybridMedia()
    {
#ifdef ROBOTWEAX_SAMPLE_TESTING
        if (test_channel_)
            test_channel_->set_send_hook_for_testing(nullptr, nullptr);
#endif
        if (returning_)
            returning_->close();
        if (fallback_)
            fallback_->close();
    }
};
}
#else
namespace samples {
class HybridMedia {
public:
    explicit HybridMedia(const Arguments& args)
    {
        for (const auto& entry : args.values)
            if (entry.first == "--hybrid"
                || entry.first.starts_with("--hybrid-"))
                throw std::runtime_error(
                    "hybrid multicast requires a static build with "
                    "experimental ACP1 and HYBRID_CONTROL enabled");
    }
    bool enabled() const
    {
        return false;
    }
    void provision(SRTSOCKET, const Arguments&, const sockaddr_in&) { }
    void attach(SRTSOCKET, const Arguments&) { }
    void delivered(const SRT_MSGCTRL&) { }
    void tick() { }
    void report() { }
};
}
#endif
