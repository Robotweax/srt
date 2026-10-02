#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace robotweax::srt::compat {

// Bytes of protected DATA plus the SRT header, excluding IP/UDP headers.
// Configuration changes discard credit. A repeated identical setting does not.
class RetransmissionBudget {
public:
    void configure(std::int64_t limit, std::uint64_t now) noexcept
    {
        if (limit == limit_)
            return;
        limit_ = limit;
        last_ = now;
        tokens_ = fraction_ = 0;
        capacity_ = limit > 0 ? std::max<std::uint64_t>(1500,
                                    static_cast<std::uint64_t>(limit) / 10)
                              : 0;
    }
    [[nodiscard]] std::int64_t limit() const noexcept
    {
        return limit_;
    }
    [[nodiscard]] bool ready(std::uint64_t bytes, std::uint64_t now) noexcept
    {
        refill(now);
        return limit_ < 0 || (limit_ > 0 && bytes <= tokens_);
    }
    void consume(std::uint64_t bytes, std::uint64_t now) noexcept
    {
        refill(now);
        if (limit_ > 0)
            tokens_ -= std::min(tokens_, bytes);
    }
    [[nodiscard]] std::optional<std::uint64_t> delay(
        std::uint64_t bytes, std::uint64_t now) noexcept
    {
        if (ready(bytes, now))
            return 0;
        if (limit_ == 0 || bytes > capacity_)
            return std::nullopt;
        // DATA datagrams are bounded to 1500 bytes, so this product fits.
        const auto numerator = (bytes - tokens_) * 1000000 - fraction_;
        const auto rate = static_cast<std::uint64_t>(limit_);
        return numerator / rate + (numerator % rate != 0 ? 1 : 0);
    }

private:
    void refill(std::uint64_t now) noexcept
    {
        if (limit_ <= 0 || now <= last_)
            return;
        const auto elapsed = now - last_;
        last_ = now;
        const auto rate = static_cast<std::uint64_t>(limit_);
        const auto seconds = elapsed / 1000000;
        const auto micros = elapsed % 1000000;
        auto room = capacity_ - tokens_;
        const auto add = [&](std::uint64_t count, std::uint64_t value) {
            if (value != 0 && count > room / value) {
                tokens_ = capacity_;
                fraction_ = 0;
                return false;
            }
            const auto credit = count * value;
            tokens_ += credit;
            room -= credit;
            return true;
        };
        if (!add(seconds, rate) || !add(micros, rate / 1000000))
            return;
        const auto fractional = micros * (rate % 1000000) + fraction_;
        if (!add(1, fractional / 1000000))
            return;
        fraction_ = room == 0 ? 0 : fractional % 1000000;
    }
    std::int64_t limit_ = -1;
    std::uint64_t capacity_ = 0;
    std::uint64_t tokens_ = 0;
    std::uint64_t fraction_ = 0;
    std::uint64_t last_ = 0;
};

} // namespace robotweax::srt::compat
