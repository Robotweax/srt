#pragma once

#include <cstddef>
#include <cstdint>

#if defined(ROBOTWEAX_SRT_SENDER_DROP_TRACE)
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

namespace robotweax::srt::diagnostics {

#if defined(ROBOTWEAX_SRT_SENDER_DROP_TRACE)
inline std::uint64_t monotonic_nanoseconds() noexcept
{
    timespec value {};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0U;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL
        + static_cast<std::uint64_t>(value.tv_nsec);
}

template <class... Arguments>
inline void append_trace(const char* format, Arguments... arguments) noexcept
{
    const char* path = std::getenv("ROBOTWEAX_SRT_SENDER_DROP_TRACE");
    if (path == nullptr || *path == '\0') {
        return;
    }
    char line[1024] {};
    const int length = std::snprintf(line, sizeof(line), format, arguments...);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(line)) {
        return;
    }
    const int descriptor = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (descriptor < 0) {
        return;
    }
    const ssize_t written =
        ::write(descriptor, line, static_cast<std::size_t>(length));
    (void)written;
    (void)::close(descriptor);
}

inline void trace_udp_submit(std::uint64_t protocol_now_microseconds,
    std::uint32_t sequence, bool retransmission, std::size_t payload_bytes,
    std::size_t buffered_packets, std::size_t packets_in_flight) noexcept
{
    append_trace("{\"event\":\"udp-submit\",\"monotonicNs\":%llu,"
                 "\"protocolNowUs\":%llu,\"sequence\":%u,"
                 "\"retransmission\":%s,\"payloadBytes\":%zu,"
                 "\"bufferedPackets\":%zu,\"packetsInFlight\":%zu}\n",
        static_cast<unsigned long long>(monotonic_nanoseconds()),
        static_cast<unsigned long long>(protocol_now_microseconds), sequence,
        retransmission ? "true" : "false", payload_bytes, buffered_packets,
        packets_in_flight);
}

inline void trace_ack(std::uint64_t protocol_now_microseconds,
    std::uint32_t next_sequence, std::int32_t progress,
    std::uint32_t first_before, std::uint32_t first_after,
    std::size_t buffered_before, std::size_t buffered_after,
    std::size_t in_flight_before, std::size_t in_flight_after) noexcept
{
    append_trace("{\"event\":\"ack\",\"monotonicNs\":%llu,"
                 "\"protocolNowUs\":%llu,\"nextSequence\":%u,"
                 "\"progressPackets\":%d,\"firstSequenceBefore\":%u,"
                 "\"firstSequenceAfter\":%u,\"bufferedPacketsBefore\":%zu,"
                 "\"bufferedPacketsAfter\":%zu,\"packetsInFlightBefore\":%zu,"
                 "\"packetsInFlightAfter\":%zu}\n",
        static_cast<unsigned long long>(monotonic_nanoseconds()),
        static_cast<unsigned long long>(protocol_now_microseconds),
        next_sequence, progress, first_before, first_after, buffered_before,
        buffered_after, in_flight_before, in_flight_after);
}

inline void trace_tlpktdrop(std::uint64_t protocol_now_microseconds,
    std::uint64_t cutoff_microseconds, std::uint32_t sequence,
    std::uint64_t enqueue_microseconds, bool previously_selected_for_send,
    bool retransmission_queued, std::size_t payload_bytes,
    std::size_t buffered_packets_before,
    std::size_t packets_in_flight_before) noexcept
{
    append_trace("{\"event\":\"tlpktdrop\",\"monotonicNs\":%llu,"
                 "\"protocolNowUs\":%llu,\"cutoffUs\":%llu,\"sequence\":%u,"
                 "\"enqueueUs\":%llu,\"ageUs\":%llu,"
                 "\"previouslySelectedForSend\":%s,\"retransmissionQueued\":%s,"
                 "\"payloadBytes\":%zu,\"bufferedPacketsBefore\":%zu,"
                 "\"packetsInFlightBefore\":%zu}\n",
        static_cast<unsigned long long>(monotonic_nanoseconds()),
        static_cast<unsigned long long>(protocol_now_microseconds),
        static_cast<unsigned long long>(cutoff_microseconds), sequence,
        static_cast<unsigned long long>(enqueue_microseconds),
        static_cast<unsigned long long>(
            protocol_now_microseconds >= enqueue_microseconds
                ? protocol_now_microseconds - enqueue_microseconds
                : 0U),
        previously_selected_for_send ? "true" : "false",
        retransmission_queued ? "true" : "false", payload_bytes,
        buffered_packets_before, packets_in_flight_before);
}

inline void trace_tlpktdrop_counter(std::uint64_t protocol_now_microseconds,
    std::uint32_t first_sequence, std::uint32_t last_sequence,
    std::size_t packet_delta, std::size_t payload_byte_delta,
    std::size_t buffered_packets_after,
    std::size_t packets_in_flight_after) noexcept
{
    append_trace("{\"event\":\"tlpktdrop-counter\",\"monotonicNs\":%llu,"
                 "\"protocolNowUs\":%llu,\"reason\":\"too-late-packet-drop\","
                 "\"firstSequence\":%u,\"lastSequence\":%u,"
                 "\"packetDelta\":%zu,\"payloadByteDelta\":%zu,"
                 "\"bufferedPacketsAfter\":%zu,\"packetsInFlightAfter\":%zu}\n",
        static_cast<unsigned long long>(monotonic_nanoseconds()),
        static_cast<unsigned long long>(protocol_now_microseconds),
        first_sequence, last_sequence, packet_delta, payload_byte_delta,
        buffered_packets_after, packets_in_flight_after);
}
#else
inline void trace_udp_submit(std::uint64_t, std::uint32_t, bool, std::size_t,
    std::size_t, std::size_t) noexcept
{
}

inline void trace_ack(std::uint64_t, std::uint32_t, std::int32_t, std::uint32_t,
    std::uint32_t, std::size_t, std::size_t, std::size_t, std::size_t) noexcept
{
}

inline void trace_tlpktdrop(std::uint64_t, std::uint64_t, std::uint32_t,
    std::uint64_t, bool, bool, std::size_t, std::size_t, std::size_t) noexcept
{
}

inline void trace_tlpktdrop_counter(std::uint64_t, std::uint32_t, std::uint32_t,
    std::size_t, std::size_t, std::size_t, std::size_t) noexcept
{
}
#endif

} // namespace robotweax::srt::diagnostics
