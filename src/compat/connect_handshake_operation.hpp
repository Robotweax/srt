#pragma once

namespace robotweax::srt::compat {

// Socket-owned lifetime boundary for a nonblocking connection handshake.
// Close requests are serialized by the concrete operation; wait() lets socket
// teardown preserve the historical guarantee that the completion callback has
// returned before an external close completes.
class ConnectHandshakeOperation {
public:
    virtual ~ConnectHandshakeOperation() = default;

    virtual void close() noexcept = 0;
    virtual void wait() noexcept = 0;
    [[nodiscard]] virtual bool running_on_current_thread() const noexcept = 0;
};

} // namespace robotweax::srt::compat
