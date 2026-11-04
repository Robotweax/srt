// SPDX-License-Identifier: MIT
#pragma once

#include <srt/srt.h>

#include <map>
#include <stdexcept>
#include <string>

namespace robotweax_app {
inline const std::map<std::string, int> arqfirst_values {
    {"0", ROBOTWEAX_SRT_FECPOLICY_ALWAYS},
    {"false", ROBOTWEAX_SRT_FECPOLICY_ALWAYS},
    {"off", ROBOTWEAX_SRT_FECPOLICY_ALWAYS},
    {"no", ROBOTWEAX_SRT_FECPOLICY_ALWAYS},
    {"1", ROBOTWEAX_SRT_FECPOLICY_ARQFIRST},
    {"true", ROBOTWEAX_SRT_FECPOLICY_ARQFIRST},
    {"on", ROBOTWEAX_SRT_FECPOLICY_ARQFIRST},
    {"yes", ROBOTWEAX_SRT_FECPOLICY_ARQFIRST},
};

inline void configure_uri(
    std::map<std::string, std::string>& options, bool output)
{
    const auto found = options.find("arqfirst");
    if (found == options.end())
        return;
    if (!output)
        throw std::invalid_argument("arqfirst is an SRT output option");
    const auto value = arqfirst_values.find(found->second);
    if (value == arqfirst_values.end())
        throw std::invalid_argument(
            "arqfirst must be yes/no, on/off, true/false or 1/0");
    if (value->second != ROBOTWEAX_SRT_FECPOLICY_ARQFIRST)
        return;
    const auto mode = options.find("mode");
    const auto type = options.find("transtype");
    if ((mode != options.end() && mode->second == "rendezvous")
        || (type != options.end() && type->second != "live"))
        throw std::invalid_argument(
            "arqfirst requires a Live SRT output in Caller/Listener mode");
    const auto group = options.find("groupconnect");
    if (group != options.end() && group->second != "0" && group->second != "no"
        && group->second != "false" && group->second != "off")
        throw std::invalid_argument(
            "arqfirst requires an individual SRT socket");
    options.try_emplace(
        "packetfilter", "fec,cols:8,rows:8,layout:even,arq:always");
}

inline void validate_mode(
    const std::map<std::string, std::string>& options, const std::string& mode)
{
    const auto found = options.find("arqfirst");
    if (found != options.end()
        && arqfirst_values.at(found->second) == ROBOTWEAX_SRT_FECPOLICY_ARQFIRST
        && mode != "caller" && mode != "listener")
        throw std::invalid_argument(
            "arqfirst requires a Live SRT output in Caller/Listener mode");
}

inline std::string display_uri(const std::string& uri)
{
    const auto query = uri.find('?');
    return query == std::string::npos
        ? uri
        : uri.substr(0, query) + "?<options omitted>";
}

inline std::string display_option(
    const std::string& name, const std::string& value)
{
    return name == "passphrase" || name == "streamid" ? "<redacted>" : value;
}

inline std::string policy_description(SRTSOCKET socket)
{
    int policy = -1;
    int policy_size = sizeof(policy);
    bool probe = true;
    int probe_size = sizeof(probe);
    if (srt_getsockflag(socket, SRTO_ROBOTWEAX_FECPOLICY, &policy, &policy_size)
            == SRT_ERROR
        || srt_getsockflag(
               socket, SRTO_ROBOTWEAX_TAILPROBE, &probe, &probe_size)
            == SRT_ERROR)
        throw std::runtime_error("cannot read back Robotweax sender policy");
    return std::string {"Robotweax ARQ-first="}
    + (policy == ROBOTWEAX_SRT_FECPOLICY_ARQFIRST ? "on" : "off")
        + " tail-probe=" + (probe ? "on" : "off");
}
} // namespace robotweax_app
