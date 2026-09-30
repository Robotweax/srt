#pragma once

#include "compat/process_state.hpp"

#include <cstdint>

namespace robotweax::srt::compat {

// Keep normal reverse-order static teardown in the process that constructed
// the value. A fork child must leave the entire inherited graph intact: even
// a no-op service destructor would still destroy its thread/lock members.
// Inline union storage avoids an extra allocation or an at-fork handler.
template <typename T> class ProcessOwned {
public:
    ProcessOwned()
        : owner_process_id_(current_process_id())
        , value_ {}
    {
    }
    ~ProcessOwned()
    {
        if (owner_process_id_ == current_process_id()) {
            value_.~T();
        }
    }
    ProcessOwned(const ProcessOwned&) = delete;
    ProcessOwned& operator=(const ProcessOwned&) = delete;

    [[nodiscard]] T& get() noexcept
    {
        return value_;
    }

private:
    const std::uint32_t owner_process_id_;
    union {
        T value_;
    };
};

} // namespace robotweax::srt::compat
