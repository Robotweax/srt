#pragma once

#include "robotweax/srt/udp.hpp"

namespace robotweax::srt::detail {

// Resolve the current outgoing route without connecting the transport channel
// or sending a datagram. The returned ephemeral port is not an SRT binding.
[[nodiscard]] EndpointResult probe_local_endpoint(IpEndpoint peer,
    std::int32_t ipv6_only, std::int32_t type_of_service,
    std::string_view bound_device) noexcept;

} // namespace robotweax::srt::detail
