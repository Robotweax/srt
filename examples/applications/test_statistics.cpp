/* SPDX-License-Identifier: MIT */
// Inject lifecycle results without linking or starting a transport runtime.
#define srt_bistats probe_bistats
#define srt_getlasterror probe_getlasterror
#define srt_getlasterror_str probe_getlasterror_str
#define srt_getsockstate probe_getsockstate
#include "common.hpp"
#include <sstream>

static int injected_error = SRT_SUCCESS;
extern "C" int probe_bistats(SRTSOCKET, SRT_TRACEBSTATS* b, int, int)
{
    if (injected_error != SRT_SUCCESS)
        return SRT_ERROR;
    b->pktSentTotal = 17;
    return 0;
}
extern "C" int probe_getlasterror(int*)
{
    return injected_error;
}
extern "C" const char* probe_getlasterror_str()
{
    return "injected failure";
}
extern "C" SRT_SOCKSTATUS probe_getsockstate(SRTSOCKET)
{
    return SRTS_CONNECTING;
}

int main()
{
    std::ostringstream output;
    struct RestoreOutput {
        std::streambuf* previous;
        ~RestoreOutput()
        {
            std::cout.rdbuf(previous);
        }
    } restore {std::cout.rdbuf(output.rdbuf())};
    for (const auto error : {SRT_ENOCONN, SRT_EINVSOCK, SRT_ECONNLOST}) {
        injected_error = error;
        output.str("");
        samples::stats(42, "path", true);
        if (output.str().find("\"event\":\"stats_unavailable\"")
                == std::string::npos
            || output.str().find("\"error_code\":" + std::to_string(error))
                == std::string::npos)
            return 1;
        bool strict_failed = false;
        try {
            samples::stats(42, "contribution");
        } catch (const std::runtime_error&) {
            strict_failed = true;
        }
        if (!strict_failed)
            return 2;
    }
    injected_error = SRT_EINVPARAM;
    bool unexpected_failed = false;
    try {
        samples::stats(42, "path", true);
    } catch (const std::runtime_error&) {
        unexpected_failed = true;
    }
    if (!unexpected_failed)
        return 3;
    injected_error = SRT_SUCCESS;
    output.str("");
    samples::stats(42, "path", true);
    if (output.str().find("\"event\":\"stats\"") == std::string::npos
        || output.str().find("\"sent\":17") == std::string::npos)
        return 4;
    return 0;
}
