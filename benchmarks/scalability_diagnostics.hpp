#pragma once

#include "srt.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

// Opt-in benchmark instrumentation, never part of the transport library.
// The observer reads only atomics; it cannot wait on an SRT runtime mutex.
class PeerDiagnostics {
public:
    enum Phase : unsigned {
        starting,
        epoll_create,
        epoll_add,
        epoll_wait,
        pending_query,
        send_call,
        receive_call,
        pacing,
        application_window,
        drain,
        epoll_remove,
        statistics,
        state_query,
        complete
    };
    struct Slot {
        std::atomic<unsigned long long> progress {0};
        std::atomic<long long> pending {-1}, pending_bytes {-1};
        std::atomic<long long> sent {-1}, received {-1}, retransmitted {-1};
        std::atomic<long long> ack_sent {-1}, ack_received {-1};
        std::atomic<long long> nak_sent {-1}, nak_received {-1};
        std::atomic<long long> send_buffer {-1}, receive_buffer {-1};
        std::atomic<long long> available_receive_bytes {-1}, state {-1};
        std::atomic<long long> logical_id {-1}, stats_at_ms {-1};
    };
    PeerDiagnostics(const char* role, const std::vector<SRTSOCKET>& sockets)
        : role_(role)
        , sockets_(sockets)
        , slots_(enabled() ? sockets.size() : 0)
        , started_(Clock::now())
        , next_stats_(started_ + std::chrono::seconds {1})
    {
        if (!slots_.empty()) {
            observer_ = std::thread([this] {
                unsigned ticks = 0;
                emit("start");
                while (!stop_.load()) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds {100});
                    if (++ticks % 10 == 0 && !stop_.load())
                        emit("sample");
                }
                emit("final");
            });
        }
    }
    ~PeerDiagnostics()
    {
        stop_.store(true);
        if (observer_.joinable())
            observer_.join();
    }
    static bool enabled() noexcept
    {
        const char* value = std::getenv("RWX_SRT_DIAGNOSTICS");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }
    void phase(Phase phase, std::size_t index = 0) noexcept
    {
        if (slots_.empty())
            return;
        operation_.store(
            (static_cast<unsigned long long>(index) << 8U) | phase);
        // A stable phase alone is not proof of a blocked call. This sequence
        // changes on every call boundary, even when phase/index repeat.
        sequence_.fetch_add(1);
    }
    void progress(std::size_t index, unsigned long long count,
        long long logical_id) noexcept
    {
        if (slots_.empty())
            return;
        slots_[index].progress.store(count);
        slots_[index].logical_id.store(logical_id);
    }
    void pending(
        std::size_t index, std::size_t packets, std::size_t bytes) noexcept
    {
        if (slots_.empty())
            return;
        slots_[index].pending.store(packets);
        slots_[index].pending_bytes.store(bytes);
    }
    void sample_stats()
    {
        if (slots_.empty() || Clock::now() < next_stats_)
            return;
        next_stats_ = Clock::now() + std::chrono::seconds {1};
        for (std::size_t i = 0; i < sockets_.size(); ++i) {
            phase(statistics, i);
            SRT_TRACEBSTATS stats {};
            const int result = srt_bstats(sockets_[i], &stats, 0);
            if (result != SRT_ERROR) {
                auto& s = slots_[i];
                s.sent.store(stats.pktSentTotal);
                s.received.store(stats.pktRecvTotal);
                s.retransmitted.store(stats.pktRetransTotal);
                s.ack_sent.store(stats.pktSentACKTotal);
                s.ack_received.store(stats.pktRecvACKTotal);
                s.nak_sent.store(stats.pktSentNAKTotal);
                s.nak_received.store(stats.pktRecvNAKTotal);
                s.send_buffer.store(stats.pktSndBuf);
                s.receive_buffer.store(stats.pktRcvBuf);
                s.available_receive_bytes.store(stats.byteAvailRcvBuf);
                s.stats_at_ms.store(elapsed());
            }
            phase(state_query, i);
            slots_[i].state.store(srt_getsockstate(sockets_[i]));
        }
        phase(starting);
    }

private:
    using Clock = std::chrono::steady_clock;
    long long elapsed() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started_)
            .count();
    }
    void emit(const char* kind)
    {
        static const char* names[] = {"starting", "epoll-create", "epoll-add",
            "epoll-wait", "pending-query", "send", "receive", "pacing",
            "application-window", "drain", "epoll-remove", "statistics",
            "state-query", "complete"};
        const auto sequence_before = sequence_.load();
        const auto operation = operation_.load();
        std::ostringstream out;
        out << "{\"event\":\"diagnostic-progress\",\"kind\":\"" << kind
            << "\",\"role\":\"" << role_ << "\",\"elapsed_ms\":" << elapsed()
            << ",\"phase\":\"" << names[operation & 255U]
            << "\",\"socket_index\":" << (operation >> 8U)
            << ",\"operation_sequence\":" << sequence_before
            << ",\"sockets\":[";
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const auto& s = slots_[i];
            out << (i ? "," : "") << "{\"index\":" << i
                << ",\"fd\":" << sockets_[i]
                << ",\"logical_id\":" << s.logical_id.load()
                << ",\"messages\":" << s.progress.load()
                << ",\"pending_packets\":" << s.pending.load()
                << ",\"pending_bytes\":" << s.pending_bytes.load()
                << ",\"sent\":" << s.sent.load()
                << ",\"received\":" << s.received.load()
                << ",\"retransmitted\":" << s.retransmitted.load()
                << ",\"ack_sent\":" << s.ack_sent.load()
                << ",\"ack_received\":" << s.ack_received.load()
                << ",\"nak_sent\":" << s.nak_sent.load()
                << ",\"nak_received\":" << s.nak_received.load()
                << ",\"send_buffer\":" << s.send_buffer.load()
                << ",\"receive_buffer\":" << s.receive_buffer.load()
                << ",\"available_receive_bytes\":"
                << s.available_receive_bytes.load()
                << ",\"state\":" << s.state.load()
                << ",\"stats_at_ms\":" << s.stats_at_ms.load() << '}';
        }
        out << "],\"operation_sequence_after\":" << sequence_.load() << "}\n";
        std::cerr << out.str() << std::flush;
    }
    const char* role_;
    const std::vector<SRTSOCKET>& sockets_;
    std::vector<Slot> slots_;
    Clock::time_point started_, next_stats_;
    std::atomic<bool> stop_ {false};
    std::atomic<unsigned long long> sequence_ {0}, operation_ {starting};
    std::thread observer_;
};
