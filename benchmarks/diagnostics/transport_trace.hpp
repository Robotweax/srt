// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Robotweax GmbH
#pragma once

// Diagnostic overlay only: NOT included by normal library builds. One bounded
// buffer per participating thread; no file writes, locks or allocation per
// event. Flush after thread exit (after the measured transfer). Missing files,
// truncated files and overflow invalidate analysis, never transport behavior.
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <unistd.h>

namespace robotweax::srt::diagnostics {
struct TraceEvent {
    std::uint64_t ns, object;
    const char* kind;
    std::uint64_t a, b, c, d;
};

inline std::atomic<unsigned> trace_thread_counter {0};

class TransportTrace {
public:
    TransportTrace() noexcept
    {
        const char* directory = std::getenv("ROBOTWEAX_TRANSPORT_TRACE_DIR");
        if (directory == nullptr || *directory == '\0') {
            return;
        }
        char path[4096];
        const int length = std::snprintf(path, sizeof(path), "%s/%ld-%u.jsonl",
            directory, static_cast<long>(getpid()), trace_thread_counter++);
        if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path)) {
            return;
        }
        file_ = std::fopen(path, "wx");
        if (file_ == nullptr) {
            return;
        }
        events_.reset(new (std::nothrow) TraceEvent[capacity]);
        std::fprintf(file_, "{\"schema\":1,\"pid\":%ld,\"capacity\":%zu}\n",
            static_cast<long>(getpid()), capacity);
        std::fflush(file_);
    }

    ~TransportTrace()
    {
        if (file_ == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < size_; ++i) {
            const auto& e = events_[i];
            std::fprintf(file_,
                "{\"ns\":%" PRIu64 ",\"object\":%" PRIu64 ",\"kind\":\"%s\","
                "\"a\":%" PRIu64 ",\"b\":%" PRIu64 ",\"c\":%" PRIu64
                ",\"d\":%" PRIu64 "}\n",
                e.ns, e.object, e.kind, e.a, e.b, e.c, e.d);
        }
        std::fprintf(file_,
            "{\"end\":true,\"events\":%zu,\"dropped\":%" PRIu64 "}\n", size_,
            dropped_);
        std::fclose(file_);
    }

    void record(const char* kind, const void* object, std::uint64_t a,
        std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept
    {
        if (file_ == nullptr) {
            return;
        }
        if (events_ == nullptr || size_ == capacity) {
            ++dropped_;
            return;
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        events_[size_++] = {static_cast<std::uint64_t>(ns),
            reinterpret_cast<std::uintptr_t>(object), kind, a, b, c, d};
    }

private:
    static constexpr std::size_t capacity = 524288; // 32 MiB per active thread
    std::unique_ptr<TraceEvent[]> events_;
    std::FILE* file_ = nullptr;
    std::size_t size_ = 0;
    std::uint64_t dropped_ = 0;
};

inline TransportTrace& thread_trace() noexcept
{
    static thread_local TransportTrace buffer;
    return buffer;
}
inline void trace(const char* kind, const void* object, std::uint64_t a = 0,
    std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0) noexcept
{
    thread_trace().record(kind, object, a, b, c, d);
}
} // namespace robotweax::srt::diagnostics

#define RWX_TRACE(...) ::robotweax::srt::diagnostics::trace(__VA_ARGS__)
