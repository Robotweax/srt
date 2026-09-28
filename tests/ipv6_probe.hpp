#pragma once

#include "robotweax/srt/udp.hpp"

// Some containers and CI sandboxes have no IPv6 loopback. Tests that need
// `::1` skip visibly instead of failing on such hosts; the probe result is
// computed once per process.
inline bool ipv6_loopback_available()
{
    static const bool available = [] {
        robotweax::srt::UdpSocket probe {robotweax::srt::IpAddressFamily::ipv6};
        return probe.valid()
            && probe.bind(robotweax::srt::IpEndpoint::ipv6_loopback())
            == robotweax::srt::Error::none;
    }();
    return available;
}

#define SKIP_WITHOUT_IPV6_LOOPBACK()                                           \
    SKIP_UNLESS(ipv6_loopback_available(), "no IPv6 loopback on this host")
