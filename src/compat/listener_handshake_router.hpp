#pragma once

#include "robotweax/srt/handshake.hpp"
#include "robotweax/srt/udp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace robotweax::srt::compat {

// UDT socket-type values are part of the shared Version-4 discovery framing,
// not an indication that this implementation supports the legacy HSv4
// establishment protocol. Keep them private to the compatibility runtime so
// HSv5 discovery and downgrade detection do not depend on the retired public
// legacy-handshake API.
inline constexpr std::uint16_t udt_stream_socket_type = 1;
inline constexpr std::uint16_t udt_datagram_socket_type = 2;

namespace detail {

// Internal primitive exposed only through the private src include path so its
// standard vectors can be tested independently from the cookie serializer.
[[nodiscard]] std::uint64_t
listener_cookie_siphash24(std::span<const std::byte> message,
                          const std::array<std::byte, 16>& key) noexcept;

} // namespace detail

// The version-4 discovery packet is shared by both protocol generations.
// Deployed HSv5 callers may put UDT_DGRAM in the extension field exactly like
// HSv4, so an initial Listener request is not sufficient to select a
// generation. Caller responses and Listener conclusions are distinguishable.
enum class ListenerHandshakeProtocol : std::uint8_t {
    invalid,
    hsv5,
    unsupported_hsv4,
};

enum class ListenerHandshakeRouteKind : std::uint8_t {
    ignore,
    send_induction_response,
    admit_conclusion,
};

struct ListenerHandshakeRoute {
    ListenerHandshakeRouteKind kind = ListenerHandshakeRouteKind::ignore;
    ListenerHandshakeProtocol protocol = ListenerHandshakeProtocol::invalid;
    HandshakeAction response{};
    HandshakeMessage reconstructed_induction{};
    std::uint32_t validated_cookie = 0;
};

class StatelessListenerHandshakeRouter {
public:
    struct Configuration {
        std::uint32_t listener_socket_id = 0;
        std::uint32_t maximum_transmission_unit = 1'500;
        std::uint32_t flow_window = 25'600;
        std::uint16_t encryption_field = 0;
        std::array<std::byte, 16> cookie_secret{};
    };

    explicit StatelessListenerHandshakeRouter(Configuration configuration) noexcept;
    ~StatelessListenerHandshakeRouter();

    StatelessListenerHandshakeRouter(const StatelessListenerHandshakeRouter&)
        = delete;
    StatelessListenerHandshakeRouter& operator=(
        const StatelessListenerHandshakeRouter&) = delete;
    StatelessListenerHandshakeRouter(StatelessListenerHandshakeRouter&&)
        = delete;
    StatelessListenerHandshakeRouter& operator=(
        StatelessListenerHandshakeRouter&&) = delete;

    // time_window is supplied by the runtime so tests need no clock and the
    // protocol component remains deterministic. Ambiguous UDT_DGRAM discovery
    // receives the normal HSv5 response. Unambiguous UDT_STREAM discovery is
    // dropped without allocating state, while an authenticated HSv4 conclusion
    // receives a stateless Version rejection. An HSv5 conclusion may use the
    // current or immediately preceding window.
    [[nodiscard]] ListenerHandshakeRoute
    route(const HandshakeMessage& message,
          IpEndpoint peer,
          std::uint64_t time_window) const noexcept;

private:
    enum class CookieDomain : std::uint8_t {
        hsv5,
        ambiguous,
    };

    [[nodiscard]] ListenerHandshakeRoute
    route_induction(const HandshakeMessage& message,
                    IpEndpoint peer,
                    std::uint64_t time_window,
                    ListenerHandshakeProtocol protocol) const noexcept;
    [[nodiscard]] ListenerHandshakeRoute
    route_conclusion(const HandshakeMessage& message,
                     IpEndpoint peer,
                     std::uint64_t time_window,
                     ListenerHandshakeProtocol protocol) const noexcept;
    [[nodiscard]] std::uint32_t
    cookie(const Handshake& packet,
           IpEndpoint peer,
           std::uint64_t time_window,
           CookieDomain domain) const noexcept;
    [[nodiscard]] bool
    matches_cookie(const Handshake& packet,
                   IpEndpoint peer,
                   std::uint64_t time_window,
                   CookieDomain domain) const noexcept;

    Configuration configuration_;
};

[[nodiscard]] ListenerHandshakeProtocol
classify_caller_induction_response(const HandshakeMessage& message) noexcept;

} // namespace robotweax::srt::compat
