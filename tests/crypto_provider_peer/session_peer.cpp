// SPDX-License-Identifier: MIT
#include <srt/srt.h>

#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(int result)
{
    if (result == SRT_ERROR)
        throw std::runtime_error(srt_getlasterror_str());
}
struct Socket {
    SRTSOCKET handle = SRT_INVALID_SOCK;
    ~Socket()
    {
        if (handle != SRT_INVALID_SOCK)
            (void)srt_close(handle);
    }
};
void set(SRTSOCKET socket, SRT_SOCKOPT option, std::int32_t value)
{
    check(srt_setsockflag(socket, option, &value, sizeof(value)));
}
constexpr unsigned messages = 512;
using Payload = std::array<char, 1316>;
Payload payload(unsigned message)
{
    Payload result {};
    for (unsigned i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>((message * 17 + i * 31) & 255);
    return result;
}
void receive(SRTSOCKET socket, unsigned message)
{
    Payload bytes {};
    check(srt_recvmsg(socket, bytes.data(), bytes.size()));
    if (bytes != payload(message))
        throw std::runtime_error("payload integrity mismatch");
}
void send(SRTSOCKET socket, unsigned message)
{
    const auto bytes = payload(message);
    const int sent = srt_sendmsg(socket, bytes.data(), bytes.size(), -1, 1);
    check(sent);
    if (sent != static_cast<int>(bytes.size()))
        throw std::runtime_error("short message");
}
}

int main(int argc, char** argv)
{
    try {
        if (argc != 6 && argc != 7 && argc != 8)
            throw std::runtime_error(
                "usage: peer caller|listener port auth "
                "key-bytes ctr|gcm [acp1 [local-rendezvous-port]]");
        check(srt_startup());
        const std::string role = argv[1];
        const bool rendezvous = role == "rdv-caller" || role == "rdv-listener";
        const bool group =
            role.rfind("broadcast-", 0) == 0 || role.rfind("backup-", 0) == 0;
        const bool backup = role.rfind("backup-", 0) == 0;
        const bool caller = role == "caller" || role == "rdv-caller"
            || role == "broadcast-caller" || role == "backup-caller";
        if (rendezvous && argc != 8)
            throw std::runtime_error("Rendezvous requires local port");
        const bool authenticated = std::stoi(argv[3]) != 0;
        const bool acp1 = argc >= 7 && std::stoi(argv[6]) != 0;
        const bool gcm = std::string(argv[5]) == "gcm";
        const char* passphrase = std::getenv("SRT_INTEROP_PASSPHRASE");
        if (!passphrase || std::strlen(passphrase) < 10)
            throw std::runtime_error(
                "synthetic passphrase environment required");
        {
            Socket socket {group && caller
                    ? srt_create_group(
                          backup ? SRT_GTYPE_BACKUP : SRT_GTYPE_BROADCAST)
                    : srt_create_socket()};
            check(socket.handle);
            check(srt_setsockflag(socket.handle, SRTO_PASSPHRASE, passphrase,
                static_cast<int>(std::strlen(passphrase))));
            set(socket.handle, SRTO_PBKEYLEN, std::stoi(argv[4]));
            set(socket.handle, SRTO_ENFORCEDENCRYPTION, 1);
            set(socket.handle, SRTO_ROBOTWEAX_SESSIONAUTH,
                authenticated ? 1 : 0);
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
            set(socket.handle, SRTO_ROBOTWEAX_ACP1_REQUIRED, acp1 ? 1 : 0);
#else
            if (acp1)
                throw std::runtime_error("ACP1 experimental build required");
#endif
            if (group) {
                if (!caller)
                    set(socket.handle, SRTO_GROUPCONNECT, 1);
                set(socket.handle, SRTO_ROBOTWEAX_PATHID_REQUIRED, 1);
                if (backup)
                    set(socket.handle, SRTO_ROBOTWEAX_STANDBYRTT_REQUIRED, 1);
            }
            if (rendezvous)
                set(socket.handle, SRTO_RENDEZVOUS, 1);
            set(socket.handle, SRTO_KMREFRESHRATE, 32);
            set(socket.handle, SRTO_KMPREANNOUNCE, 8);
            set(socket.handle, SRTO_RCVLATENCY, 0);
            set(socket.handle, SRTO_PEERLATENCY, 0);
            set(socket.handle, SRTO_CONNTIMEO, 6000);
            set(socket.handle, SRTO_RCVTIMEO, 6000);
            set(socket.handle, SRTO_SNDTIMEO, 6000);
            // Group members use asynchronous I/O. Immediate close is required
            // for this replacement probe; asynchronous linger deliberately
            // keeps the removed path alive while its queued traffic drains.
            const linger draining {group ? 0 : 1, group ? 0 : 2};
            check(srt_setsockflag(
                socket.handle, SRTO_LINGER, &draining, sizeof(draining)));
#ifdef ENABLE_AEAD_API_PREVIEW
            set(socket.handle, SRTO_CRYPTOMODE, gcm ? 2 : 1);
#else
            if (gcm)
                throw std::runtime_error("GCM preview required");
#endif
            sockaddr_in address {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port =
                htons(static_cast<unsigned short>(std::stoi(argv[2])));
            Socket accepted;
            SRTSOCKET transport = socket.handle;
            const auto join = [&](const char* path) {
                auto* config = srt_create_config();
                if (!config)
                    throw std::runtime_error("group config allocation");
                const auto cleanup = [&] {
                    srt_delete_config(config);
                };
                const auto added = srt_config_add(
                    config, SRTO_ROBOTWEAX_PATHID, path, std::strlen(path));
                if (added == SRT_ERROR) {
                    cleanup();
                    check(added);
                }
                auto endpoint = srt_prepare_endpoint(nullptr,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address));
                endpoint.config = config;
                const auto connected =
                    srt_connect_group(socket.handle, &endpoint, 1);
                cleanup();
                check(connected);
                return endpoint.id;
            };
            SRTSOCKET first_member = SRT_INVALID_SOCK;
            if (group && caller) {
                first_member = join("provider-path-a");
                (void)join("provider-path-b");
            } else if (rendezvous) {
                auto local = address;
                local.sin_port =
                    htons(static_cast<unsigned short>(std::stoi(argv[7])));
                check(srt_bind(socket.handle,
                    reinterpret_cast<const sockaddr*>(&local), sizeof(local)));
                if (!caller)
                    std::cout << "ready\n" << std::flush;
                check(srt_connect(socket.handle,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)));
            } else if (caller) {
                check(srt_connect(socket.handle,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)));
            } else {
                check(srt_bind(socket.handle,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)));
                check(srt_listen(socket.handle, group ? 4 : 1));
                std::cout << "ready\n" << std::flush;
                accepted.handle = srt_accept(socket.handle, nullptr, nullptr);
                check(accepted.handle);
                transport = accepted.handle;
            }
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_ACP1
            bool active = false;
            int active_size = sizeof(active);
            check(srt_getsockflag(
                transport, SRTO_ROBOTWEAX_ACP1_ACTIVE, &active, &active_size));
            if (active != acp1)
                throw std::runtime_error("ACP1 activation mismatch");
#endif
            for (unsigned n = 0; n < messages; ++n) {
                try {
                    if (caller) {
                        if (group && n == messages / 2) {
                            check(srt_close(first_member));
                            (void)join("provider-path-a");
                        }
                        send(transport, n);
                        receive(transport, n);
                    } else {
                        receive(transport, n);
                        send(transport, n);
                    }
                } catch (const std::exception& error) {
                    if (group) {
                        std::array<SRT_SOCKGROUPDATA, 16> members {};
                        std::size_t count = members.size();
                        if (srt_group_data(transport, members.data(), &count)
                            >= 0) {
                            for (std::size_t i = 0; i < count; ++i) {
                                SRT_TRACEBSTATS stats {};
                                (void)srt_bstats(members[i].id, &stats, 0);
                                std::cerr
                                    << role << " member=" << members[i].id
                                    << " state=" << members[i].sockstate
                                    << " sent=" << stats.pktSentTotal
                                    << " recv=" << stats.pktRecvTotal
                                    << " undecrypt="
                                    << stats.pktRcvUndecryptTotal
                                    << " dropped=" << stats.pktRcvDropTotal
                                    << " belated=" << stats.pktRcvBelated
                                    << " sndbuf=" << stats.byteSndBuf
                                    << " rcvbuf=" << stats.byteRcvBuf << '\n';
                            }
                        }
                    }
                    throw std::runtime_error(role + " message "
                        + std::to_string(n) + ": " + error.what());
                }
            }
            std::int32_t state = 0;
            int state_size = sizeof(state);
            check(srt_getsockflag(
                transport, SRTO_SNDKMSTATE, &state, &state_size));
            if (state != SRT_KM_S_SECURED)
                throw std::runtime_error("sender keys are not secured");
#ifdef ENABLE_AEAD_API_PREVIEW
            state_size = sizeof(state);
            check(srt_getsockflag(
                transport, SRTO_CRYPTOMODE, &state, &state_size));
            if (state != (gcm ? 2 : 1))
                throw std::runtime_error("negotiated cipher mode mismatch");
#endif
            std::int32_t backend = 0;
            int size = sizeof(backend);
            check(srt_getsockflag(
                transport, SRTO_ROBOTWEAX_CRYPTO_BACKEND, &backend, &size));
            std::cout << "{\"backend\":" << backend
                      << ",\"messages\":" << messages << ",\"session_auth\":"
                      << (authenticated ? "true" : "false") << ",\"mode\":\""
                      << (gcm ? "gcm" : "ctr")
                      << "\",\"acp1\":" << (acp1 ? "true" : "false")
                      << ",\"rendezvous\":" << (rendezvous ? "true" : "false")
                      << ",\"group\":\""
                      << (group ? backup ? "backup" : "broadcast" : "none")
                      << "\",\"integrity\":true}\n";
        }
        check(srt_cleanup());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        (void)srt_cleanup();
        return 1;
    }
}
