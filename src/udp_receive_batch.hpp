#pragma once

#include "robotweax/srt/udp.hpp"

#if defined(ROBOTWEAX_SRT_EXPERIMENTAL_UDP_BATCHING)

#include <cstddef>
#include <span>
#include <sys/socket.h>

namespace robotweax::srt::detail {

// The two halves of UdpSocket::receive_batch around its recvmmsg call. They
// are separate so that a test can hand complete_receive_batch a kernel result
// that recvmmsg does not produce on its own.

// Points message i at slot i of storage and at peers[i], and clears peers[i].
// All spans hold at least `count` elements; storage holds count * slot_size.
void prepare_receive_batch(std::span<std::byte> storage, std::size_t slot_size,
    std::size_t count, std::span<mmsghdr> messages, std::span<iovec> buffers,
    std::span<sockaddr_storage> peers) noexcept;

// Converts the first `consumed` kernel messages into per-datagram results.
// A reported length never exceeds slot_size.
void complete_receive_batch(std::span<const mmsghdr> messages,
    std::span<const sockaddr_storage> peers, std::size_t slot_size,
    std::size_t consumed, std::span<UdpIoResult> results) noexcept;

} // namespace robotweax::srt::detail

#endif
