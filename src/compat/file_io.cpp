#include "compat/file_io.hpp"

#include "compat/error_state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace robotweax::srt::compat {
namespace {

[[nodiscard]] std::int64_t fail(
    SRT_ERRNO error, int system_error = 0) noexcept
{
    set_last_error(error, system_error);
    return SRT_ERROR;
}

struct FileRuntime {
    std::shared_ptr<ConnectionRuntime> runtime;
    bool file_mode = false;
};

[[nodiscard]] bool connected_runtime(
    SocketRecord* socket, FileRuntime& result) noexcept
{
    if (socket == nullptr) {
        (void)fail(SRT_EINVSOCK);
        return false;
    }

    std::lock_guard lock(socket->mutex);
    if (socket->state == SRTS_CLOSED) {
        (void)fail(SRT_EINVSOCK);
        return false;
    }
    if (socket->state == SRTS_BROKEN) {
        (void)fail(SRT_ECONNLOST);
        return false;
    }
    if (socket->state != SRTS_CONNECTED
        || socket->runtime == nullptr) {
        (void)fail(SRT_ENOCONN);
        return false;
    }
    result.runtime = socket->runtime;
    result.file_mode =
        socket->public_options.transmission_type == SRTT_FILE
        && !socket->public_options.message_api
        && !socket->public_options.tsbpd_mode;
    return true;
}

[[nodiscard]] std::int64_t map_send_failure(
    const MessageIoResult& result) noexcept
{
    switch (result.status) {
    case MessageIoStatus::local_closed:
        return fail(SRT_ESCLOSED);
    case MessageIoStatus::peer_closed:
    case MessageIoStatus::broken:
        return fail(SRT_ECONNLOST, result.system_error);
    case MessageIoStatus::peer_error:
        return fail(SRT_EPEERERR);
    case MessageIoStatus::timeout:
        return fail(SRT_ETIMEOUT);
    case MessageIoStatus::would_block:
        return fail(SRT_EASYNCSND);
    case MessageIoStatus::buffer_too_small:
        return fail(SRT_ENOBUF);
    case MessageIoStatus::invalid_state:
        return fail(SRT_EINVOP);
    case MessageIoStatus::success:
        break;
    }
    return fail(SRT_EUNKNOWN);
}

[[nodiscard]] std::int64_t map_receive_failure(
    const MessageIoResult& result) noexcept
{
    switch (result.status) {
    case MessageIoStatus::local_closed:
        return fail(SRT_ESCLOSED);
    case MessageIoStatus::peer_closed:
    case MessageIoStatus::broken:
        return fail(SRT_ECONNLOST, result.system_error);
    case MessageIoStatus::peer_error:
        return fail(SRT_EPEERERR);
    case MessageIoStatus::timeout:
        return fail(SRT_ETIMEOUT);
    case MessageIoStatus::would_block:
        return fail(SRT_EASYNCRCV);
    case MessageIoStatus::buffer_too_small:
        return fail(SRT_ENOBUF);
    case MessageIoStatus::invalid_state:
        return fail(SRT_EINVOP);
    case MessageIoStatus::success:
        break;
    }
    return fail(SRT_EUNKNOWN);
}

[[nodiscard]] bool valid_common_arguments(
    const char* path,
    const std::int64_t* offset,
    int block_size) noexcept
{
    if (path == nullptr || offset == nullptr || block_size <= 0) {
        (void)fail(SRT_EINVPARAM);
        return false;
    }
    return true;
}

[[nodiscard]] std::int64_t file_write_failure(
    const std::shared_ptr<ConnectionRuntime>& runtime) noexcept
{
    (void)runtime->report_peer_error(SRT_EFILE);
    return fail(SRT_EWRPERM);
}

} // namespace

std::int64_t send_file(
    SocketRecord* socket,
    const char* path,
    std::int64_t* offset,
    std::int64_t size,
    int block_size) noexcept
{
    if (!valid_common_arguments(path, offset, block_size)) {
        return SRT_ERROR;
    }

    try {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
            return fail(SRT_ERDPERM);
        }
        FileRuntime context;
        if (!connected_runtime(socket, context)) {
            return SRT_ERROR;
        }
        if (size <= 0 && size != -1) {
            return 0;
        }
        if (!context.file_mode) {
            return fail(SRT_EINVALBUFFERAPI);
        }
        if (*offset < 0) {
            return fail(SRT_EINVRDOFF);
        }

        input.seekg(0, std::ios::end);
        const auto end_position = input.tellg();
        if (end_position < 0
            || static_cast<std::uint64_t>(end_position)
                > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
            || *offset > static_cast<std::int64_t>(end_position)) {
            return fail(SRT_EINVRDOFF);
        }
        const std::int64_t available =
            static_cast<std::int64_t>(end_position) - *offset;
        std::int64_t remaining =
            size == -1 ? available : std::min(size, available);
        input.seekg(static_cast<std::streamoff>(*offset), std::ios::beg);
        if (!input.good()) {
            return fail(SRT_EINVRDOFF);
        }
        if (remaining == 0) {
            return 0;
        }

        const auto buffer_size = static_cast<std::size_t>(
            std::min<std::int64_t>(remaining, block_size));
        std::vector<std::byte> buffer(buffer_size);
        std::int64_t transferred = 0;
        while (remaining > 0) {
            const auto requested = static_cast<std::size_t>(
                std::min<std::int64_t>(
                    remaining,
                    static_cast<std::int64_t>(buffer.size())));
            input.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(requested));
            const auto read_count = input.gcount();
            if (read_count <= 0) {
                if (input.eof()) {
                    break;
                }
                return fail(SRT_ERDPERM);
            }

            std::size_t queued = 0;
            const auto read_size =
                static_cast<std::size_t>(read_count);
            while (queued < read_size) {
                const auto result = context.runtime->queue_stream(
                    std::span{buffer}.subspan(
                        queued, read_size - queued),
                    true, -1);
                if (result.status != MessageIoStatus::success
                    || result.bytes == 0U) {
                    return map_send_failure(result);
                }
                queued += result.bytes;
                transferred += static_cast<std::int64_t>(
                    result.bytes);
                remaining -= static_cast<std::int64_t>(
                    result.bytes);
                *offset += static_cast<std::int64_t>(
                    result.bytes);
            }
        }
        if (input.bad()) {
            return fail(SRT_ERDPERM);
        }
        return transferred;
    } catch (const std::bad_alloc&) {
        return fail(SRT_ENOBUF);
    } catch (...) {
        return fail(SRT_EFILE);
    }
}

std::int64_t receive_file(
    SocketRecord* socket,
    const char* path,
    std::int64_t* offset,
    std::int64_t size,
    int block_size) noexcept
{
    if (!valid_common_arguments(path, offset, block_size)) {
        return SRT_ERROR;
    }

    try {
        std::ofstream output(
            path, std::ios::binary | std::ios::out);
        if (!output.is_open()) {
            return fail(SRT_EWRPERM);
        }
        return receive_file_stream(
            socket, output, offset, size, block_size);
    } catch (const std::bad_alloc&) {
        return fail(SRT_ENOBUF);
    } catch (...) {
        return fail(SRT_EFILE);
    }
}

std::int64_t receive_file_stream(
    SocketRecord* socket,
    std::ostream& output,
    std::int64_t* offset,
    std::int64_t size,
    int block_size) noexcept
{
    if (offset == nullptr || block_size <= 0) {
        return fail(SRT_EINVPARAM);
    }

    try {
        FileRuntime context;
        if (!connected_runtime(socket, context)) {
            return SRT_ERROR;
        }
        if (size <= 0) {
            return 0;
        }
        if (!context.file_mode) {
            return fail(SRT_EINVALBUFFERAPI);
        }
        if (*offset < 0) {
            return fail(SRT_EINVWROFF);
        }
        if (size > std::numeric_limits<std::int64_t>::max()
                - *offset
            || static_cast<std::uint64_t>(*offset)
                > static_cast<std::uint64_t>(
                    std::numeric_limits<std::streamoff>::max())) {
            return fail(SRT_EINVWROFF);
        }
        if (*offset > 0) {
            try {
                output.seekp(
                    static_cast<std::streamoff>(*offset),
                    std::ios::beg);
            } catch (...) {
                return fail(SRT_EINVWROFF);
            }
            if (!output.good()) {
                return fail(SRT_EINVWROFF);
            }
        }
        if (!output.good()) {
            return file_write_failure(context.runtime);
        }

        const auto buffer_size = static_cast<std::size_t>(
            std::min<std::int64_t>(size, block_size));
        std::vector<std::byte> buffer(buffer_size);
        std::int64_t remaining = size;
        std::int64_t transferred = 0;
        while (remaining > 0) {
            const auto requested = static_cast<std::size_t>(
                std::min<std::int64_t>(
                    remaining,
                    static_cast<std::int64_t>(buffer.size())));
            const auto result = context.runtime->receive_stream(
                std::span{buffer}.first(requested),
                true, -1);
            if (result.status != MessageIoStatus::success) {
                return map_receive_failure(result);
            }
            if (result.bytes == 0U) {
                break;
            }

            try {
                output.write(
                    reinterpret_cast<const char*>(buffer.data()),
                    static_cast<std::streamsize>(result.bytes));
            } catch (...) {
                return file_write_failure(context.runtime);
            }
            if (!output.good()) {
                return file_write_failure(context.runtime);
            }
            remaining -= static_cast<std::int64_t>(
                result.bytes);
            transferred += static_cast<std::int64_t>(
                result.bytes);
            *offset += static_cast<std::int64_t>(
                result.bytes);
        }
        try {
            output.flush();
        } catch (...) {
            return file_write_failure(context.runtime);
        }
        if (!output.good()) {
            return file_write_failure(context.runtime);
        }
        return transferred;
    } catch (const std::bad_alloc&) {
        return fail(SRT_ENOBUF);
    } catch (...) {
        return fail(SRT_EFILE);
    }
}

} // namespace robotweax::srt::compat
