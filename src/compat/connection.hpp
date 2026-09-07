#pragma once

#include "compat/listener_accept_queue.hpp"
#include "compat/socket_registry.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace robotweax::srt::compat {

class ListenerHandshakeActor;
class ListenerConnectionSetupActor;
class RuntimeWorkExecutor;
struct ListenerHandshakeAdmission;
struct ListenerHandshakeActorResult;
struct ListenerConnectionSetupActorResult;

class ListenerRuntime : public std::enable_shared_from_this<ListenerRuntime> {
public:
    ListenerRuntime(
        std::weak_ptr<SocketRecord> listener,
        std::size_t capacity);
    ~ListenerRuntime();

    ListenerRuntime(const ListenerRuntime&) = delete;
    ListenerRuntime& operator=(const ListenerRuntime&) = delete;

    [[nodiscard]] bool start() noexcept;
    void close() noexcept;
    [[nodiscard]] SRTSOCKET pop(
        bool blocking, IpEndpoint& peer) noexcept;

private:
    struct ResultContext;
    struct SetupContext;

    static void result_ready(void* context) noexcept;
    static void run_result_task(void* context) noexcept;

    void request_result_work() noexcept;
    void drain_results() noexcept;
    [[nodiscard]] bool process_result(
        const ListenerHandshakeActorResult& result) noexcept;
    [[nodiscard]] bool process_setup_result(
        const std::shared_ptr<SetupContext>& context,
        const ListenerConnectionSetupActorResult& result) noexcept;
    [[nodiscard]] bool finish_setup_context(
        const std::shared_ptr<SetupContext>& context, bool connected,
        SRT_ERRNO error = SRT_SUCCESS, int system_error = 0) noexcept;
    void abandon_setup_context(
        const std::shared_ptr<SetupContext>& context) noexcept;
    void finish_result_consumer() noexcept;
    [[nodiscard]] SRTSOCKET start_admitted_socket(
        std::shared_ptr<SocketRecord> listener,
        const ListenerHandshakeAdmission& admission) noexcept;
    [[nodiscard]] ListenerAcceptPublishStatus publish(
        ListenerAcceptedConnection connection) noexcept;

    std::weak_ptr<SocketRecord> listener_;
    ListenerAcceptQueue accepts_;
    std::mutex mutex_;
    std::condition_variable result_idle_;
    bool closed_ = false;
    bool result_work_scheduled_ = false;
    bool result_work_pending_ = false;
    bool result_work_active_ = false;
    std::thread::id result_work_thread_ {};
    std::shared_ptr<ListenerHandshakeActor> handshake_actor_;
    std::shared_ptr<ListenerConnectionSetupActor> setup_actor_;
    std::shared_ptr<SetupContext> setup_context_;
    std::shared_ptr<RuntimeWorkExecutor> work_executor_;
    std::shared_ptr<ResultContext> result_context_;
};

[[nodiscard]] int listen_socket(
    std::shared_ptr<SocketRecord> socket, int backlog) noexcept;
[[nodiscard]] int set_listen_callback(
    std::shared_ptr<SocketRecord> socket,
    srt_listen_callback_fn* callback,
    void* opaque) noexcept;
[[nodiscard]] int set_connect_callback(
    std::shared_ptr<SocketRecord> socket,
    srt_connect_callback_fn* callback,
    void* opaque) noexcept;
[[nodiscard]] int connect_socket(
    std::shared_ptr<SocketRecord> socket,
    const sockaddr* name, int name_size,
    std::int32_t forced_initial_sequence = SRT_SEQNO_NONE) noexcept;
[[nodiscard]] int connect_bound_socket(
    std::shared_ptr<SocketRecord> socket,
    const sockaddr* source, const sockaddr* target,
    int name_size) noexcept;
[[nodiscard]] SRTSOCKET accept_socket(
    std::shared_ptr<SocketRecord> listener,
    sockaddr* peer_name,
    int* peer_name_size) noexcept;
[[nodiscard]] SRTSOCKET accept_bond_sockets(
    const SRTSOCKET* listeners, int listener_count,
    std::int64_t timeout_milliseconds) noexcept;
[[nodiscard]] int get_peer_name(
    SocketRecord& socket, sockaddr* name, int* name_size) noexcept;

} // namespace robotweax::srt::compat
