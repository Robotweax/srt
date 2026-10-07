#pragma once

#include "robotweax/srt/udp.hpp"

#include <algorithm>
#include <array>
#include <chrono>

namespace robotweax::srt::compat {

// Charged before creating encrypted accepted sockets, including failed and
// repeated cookie-valid conclusions. Ports, cookies and salts cannot reset it.
// The owner serializes access; the table never allocates or evicts active debt.
class ListenerCryptoBudget {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr unsigned global_burst = 128;
    static constexpr unsigned source_burst = 32;
    static constexpr unsigned global_per_second = 64;
    static constexpr unsigned source_per_second = 16;

    [[nodiscard]] bool admit(IpEndpoint source, Clock::time_point now) noexcept
    {
        source.port = 0;
        if (source.is_ipv4_mapped_ipv6()) {
            IpEndpoint normalized;
            std::copy_n(
                source.address.begin() + 12, 4, normalized.address.begin());
            source = normalized;
        }
        refill(global_, now, global_burst, global_per_second);
        if (global_.tokens < 1)
            return false;
        Entry* selected = nullptr;
        Entry* reusable = nullptr;
        for (auto& entry : sources_) {
            if (entry.used && entry.source == source) {
                selected = &entry;
                break;
            }
            // Reclaim only fully replenished entries, so address churn cannot
            // grant a second burst to a source with outstanding debt.
            if (!entry.used
                || (now >= entry.bucket.updated
                    && now - entry.bucket.updated >= std::chrono::seconds {2}))
                reusable = &entry;
        }
        if (selected == nullptr) {
            if (reusable == nullptr)
                return false;
            selected = reusable;
            *selected = Entry {
                .source = source, .bucket = {source_burst, now}, .used = true};
        }
        refill(selected->bucket, now, source_burst, source_per_second);
        if (selected->bucket.tokens < 1)
            return false;
        --selected->bucket.tokens;
        --global_.tokens;
        return true;
    }

private:
    struct Bucket {
        double tokens;
        Clock::time_point updated;
    };
    struct Entry {
        IpEndpoint source {};
        Bucket bucket {};
        bool used = false;
    };
    static void refill(Bucket& bucket, Clock::time_point now, unsigned capacity,
        unsigned rate) noexcept
    {
        if (now <= bucket.updated)
            return;
        const double elapsed =
            std::chrono::duration<double>(now - bucket.updated).count();
        bucket.tokens = std::min(
            static_cast<double>(capacity), bucket.tokens + elapsed * rate);
        bucket.updated = now;
    }
    Bucket global_ {global_burst, {}};
    std::array<Entry, global_burst> sources_ {};
};

} // namespace robotweax::srt::compat
