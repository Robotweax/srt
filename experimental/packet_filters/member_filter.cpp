#include "member_filter.hpp"

#include <algorithm>
#include <limits>

namespace robotweax::srt::extensions {

Error MemberFilterRuntime::start(const CompiledFilter& filter,
    std::uint32_t local_socket, std::uint32_t peer_socket,
    SequenceNumber send_initial, SequenceNumber receive_initial,
    std::size_t send_capacity, std::size_t receive_capacity,
    std::size_t maximum_payload) noexcept
{
    if (started_)
        return Error::invalid_state;
    started_ = true;
    local_socket_ = local_socket;
    peer_socket_ = peer_socket;
    maximum_payload_ = maximum_payload;
    const auto& descriptor = filter.implementation.descriptor;
    if (!local_socket || !peer_socket || !maximum_payload
        || maximum_payload
            > maximum_data_payload_size - experimental_member_filter_overhead
        || !send_capacity || send_capacity >= SequenceNumber::half_range
        || !receive_capacity || receive_capacity >= SequenceNumber::half_range
        || descriptor.scope != FilterScope::member_transport
        || descriptor.feedback != FeedbackRequirement::none
        || descriptor.state_bytes_per_receiver != 0)
        return failure_ = Error::unsupported;
    const FilterLimits limits {.maximum_members = 1,
        .maximum_receivers = 0,
        .maximum_state_bytes = maximum_state,
        .maximum_outputs_per_batch = maximum_outputs,
        .maximum_output_bytes_per_batch = maximum_outputs * maximum_symbol};
    for (auto* direction : {&sender_, &receiver_}) {
        const bool sending = direction == &sender_;
        direction->first = sending ? send_initial : receive_initial;
        direction->paths[0] = {
            {1, 1}, maximum_payload + packet_header_size, {}};
        direction->environment = {
            .context = {.session = {local_socket, 1},
                .stream = {1, 1},
                .configuration_generation = 1,
                .direction = static_cast<std::uint8_t>(sending ? 0 : 1)},
            .paths = direction->paths,
            .first_sequence = SequenceNumber::modulus,
            .end_sequence = SequenceNumber::modulus
                + (sending ? send_capacity : receive_capacity),
            .maximum_recovered_payload_bytes =
                maximum_payload + packet_header_size};
        failure_ = direction->instance.start(filter.implementation, limits,
            {.scope = FilterScope::member_transport, .member = {1, 1}},
            direction->environment, direction->state, direction->outputs,
            direction->bytes);
        if (failure_ != Error::none)
            return failure_;
    }
    return Error::none;
}

std::optional<std::uint64_t> MemberFilterRuntime::position(
    const Direction& direction, SequenceNumber sequence) const noexcept
{
    const auto offset = sequence.distance_from(direction.first);
    if (offset < 0
        || static_cast<std::uint64_t>(offset)
            >= direction.environment.end_sequence
                - direction.environment.first_sequence)
        return std::nullopt;
    return direction.environment.first_sequence
        + static_cast<std::uint64_t>(offset);
}

Error MemberFilterRuntime::advance(Direction& direction, SequenceNumber first,
    std::uint64_t now, bool sender) noexcept
{
    const auto distance = first.distance_from(direction.first);
    if (distance < 0)
        return failure_ = Error::invalid_state;
    if (!distance)
        return Error::none;
    auto next = direction.environment;
    if (next.end_sequence > (std::numeric_limits<std::uint64_t>::max)()
            - static_cast<std::uint64_t>(distance))
        return failure_ = Error::invalid_state;
    next.first_sequence += static_cast<std::uint64_t>(distance);
    next.end_sequence += static_cast<std::uint64_t>(distance);
    const auto result = direction.instance.update_environment(next, now);
    failure_ = sender ? queue(result)
                      : (result.error != Error::none      ? result.error
                                : !result.outputs.empty() ? Error::unsupported
                                                          : Error::none);
    if (failure_ == Error::none) {
        direction.environment = next;
        direction.first = first;
    }
    return failure_;
}

bool MemberFilterRuntime::valid_source(
    const PacketView& packet, bool sender) const noexcept
{
    return packet.kind == PacketKind::data && packet.data.message_number != 0
        && packet.data.encryption_key == EncryptionKey::none
        && !packet.payload.empty() && packet.payload.size() <= maximum_payload_
        && packet.data.destination_socket_id
        == (sender ? peer_socket_ : local_socket_);
}

Error MemberFilterRuntime::queue(const FilterDispatchResult& result) noexcept
{
    if (result.error != Error::none)
        return failure_ = result.error;
    if (result.outputs.empty())
        return Error::none;
    if (result.outputs.size() > maximum_outputs - pending_count_)
        return failure_ = Error::buffer_too_small;
    std::array<Datagram, maximum_outputs> batch;
    std::size_t charge = 0;
    for (std::size_t index = 0; index < result.outputs.size(); ++index) {
        const auto& output = result.outputs[index];
        if (output.kind != FilterOutputKind::repair || output.receiver.valid()
            || output.payload.size() > maximum_symbol)
            return failure_ = Error::unsupported;
        // The generic dispatcher already validated identity, sequence window,
        // destination and batch budgets. This binding owns wire framing.
        auto& item = batch[index];
        const auto size = output.payload.size();
        std::array<std::byte, maximum_symbol + 7> framed;
        framed[0] = std::byte {1};
        framed[1] =
            std::byte {0}; // Repair; feedback is not enabled by this binding.
        framed[2] = static_cast<std::byte>(size >> 8U);
        framed[3] = static_cast<std::byte>(size & 255U);
        std::copy(
            output.payload.begin(), output.payload.end(), framed.begin() + 4);
        const auto body_size = (size + 7U) & ~std::size_t {3};
        std::fill(framed.begin() + static_cast<std::ptrdiff_t>(4 + size),
            framed.begin() + static_cast<std::ptrdiff_t>(body_size),
            std::byte {});
        const auto offset =
            output.sequence - sender_.environment.first_sequence;
        const auto sequence =
            sender_.first.advanced(static_cast<std::uint32_t>(offset));
        const auto encoded =
            encode_packet({.kind = PacketKind::control,
                              .control = {.type = ControlType::user_defined,
                                  .subtype = member_filter_subtype,
                                  .type_specific = sequence.value(),
                                  .timestamp = {},
                                  .destination_socket_id = peer_socket_},
                              .payload = std::span {framed}.first(body_size)},
                item.bytes);
        if (!encoded)
            return failure_ = encoded.error;
        item.size = encoded.bytes_written;
        charge += item.size;
    }
    if (charge > credit_)
        return failure_ = Error::invalid_state;
    // Reserve the entire batch and its amplification credit before publishing.
    for (std::size_t index = 0; index < result.outputs.size(); ++index) {
        auto& destination =
            pending_[(head_ + pending_count_ + index) % maximum_outputs];
        destination.size = batch[index].size;
        std::copy_n(batch[index].bytes.begin(), destination.size,
            destination.bytes.begin());
    }
    pending_count_ += result.outputs.size();
    credit_ -= charge;
    return Error::none;
}

Error MemberFilterRuntime::source_sent(
    const PacketView& packet, SequenceNumber first, std::uint64_t now) noexcept
{
    if (!started_ || failure_ != Error::none)
        return failure_ == Error::none ? Error::invalid_state : failure_;
    if (!valid_source(packet, true))
        return failure_ = Error::invalid_state;
    if (advance(sender_, first, now, true) != Error::none)
        return failure_;
    const auto sequence = position(sender_, packet.data.sequence);
    if (!sequence)
        return failure_ = Error::invalid_state;
    std::array<std::byte, maximum_symbol> symbol {};
    const auto encoded = encode_packet({.kind = PacketKind::data,
                                           .data = packet.data,
                                           .payload = packet.payload},
        symbol);
    if (!encoded)
        return failure_ = encoded.error;
    // One submitted source funds at most its framed size in repair traffic.
    // Unused credit is capped; all queued repair is paced by the core.
    credit_ = std::min(maximum_outputs * maximum_datagram,
        credit_ + ((encoded.bytes_written + 7U) & ~std::size_t {3})
            + packet_header_size);
    return queue(sender_.instance.process(
        {.context = sender_.environment.context,
            .domain = SequenceDomain::member_transport,
            .sequence = *sequence,
            .origin = {1, 1},
            .member_sequence = packet.data.sequence.value(),
            .payload = std::span {symbol}.first(encoded.bytes_written)},
        now));
}

MemberFilterReceiveResult MemberFilterRuntime::recover(
    const FilterDispatchResult& result) noexcept
{
    if (result.error != Error::none)
        return {.error = failure_ = result.error};
    for (std::size_t index = 0; index < result.outputs.size(); ++index) {
        const auto& output = result.outputs[index];
        const auto decoded = decode_packet(output.payload);
        if (output.kind != FilterOutputKind::recovered_data || !decoded
            || !valid_source(decoded.packet, false)
            || position(receiver_, decoded.packet.data.sequence)
                != output.sequence)
            return {.error = failure_ = Error::invalid_state};
        recovered_[index] = decoded.packet;
    }
    return {.admitted = true,
        .recovered = std::span {recovered_}.first(result.outputs.size())};
}

MemberFilterReceiveResult MemberFilterRuntime::receive_symbol(
    FilterInputKind kind, SequenceNumber sequence,
    std::span<const std::byte> symbol, SequenceNumber first,
    std::uint64_t now) noexcept
{
    if (!started_ || failure_ != Error::none)
        return {
            .error = failure_ == Error::none ? Error::invalid_state : failure_};
    if (advance(receiver_, first, now, false) != Error::none)
        return {.error = failure_};
    const auto lifted = position(receiver_, sequence);
    if (!lifted || symbol.empty()
        || symbol.size() > maximum_payload_ + packet_header_size)
        return {};
    return recover(receiver_.instance.process(
        {.context = receiver_.environment.context,
            .domain = SequenceDomain::member_transport,
            .sequence = *lifted,
            .origin = {1, 1},
            .member_sequence = sequence.value(),
            .payload = symbol,
            .kind = kind},
        now));
}

MemberFilterReceiveResult MemberFilterRuntime::receive_source(
    const PacketView& packet, SequenceNumber first, std::uint64_t now) noexcept
{
    if (!started_ || failure_ != Error::none)
        return {
            .error = failure_ == Error::none ? Error::invalid_state : failure_};
    if (!valid_source(packet, false))
        return {};
    std::array<std::byte, maximum_symbol> symbol {};
    const auto encoded = encode_packet({.kind = PacketKind::data,
                                           .data = packet.data,
                                           .payload = packet.payload},
        symbol);
    if (!encoded)
        return {};
    return receive_symbol(FilterInputKind::source, packet.data.sequence,
        std::span {symbol}.first(encoded.bytes_written), first, now);
}

MemberFilterReceiveResult MemberFilterRuntime::receive_repair(
    const PacketView& packet, SequenceNumber first, std::uint64_t now) noexcept
{
    if (!started_ || failure_ != Error::none)
        return {
            .error = failure_ == Error::none ? Error::invalid_state : failure_};
    if (packet.kind != PacketKind::control
        || packet.control.type != ControlType::user_defined
        || packet.control.subtype != member_filter_subtype
        || packet.control.destination_socket_id != local_socket_
        || packet.control.type_specific > SequenceNumber::mask
        || !has_valid_control_payload_shape(packet.payload)
        || packet.payload.size() < 8 || packet.payload[0] != std::byte {1}
        || packet.payload[1] != std::byte {0})
        return {};
    const auto size = (std::to_integer<std::size_t>(packet.payload[2]) << 8U)
        | std::to_integer<std::size_t>(packet.payload[3]);
    if (!size || size > maximum_payload_ + packet_header_size
        || packet.payload.size() != ((size + 7U) & ~std::size_t {3}))
        return {};
    for (const auto byte : packet.payload.subspan(4 + size))
        if (byte != std::byte {})
            return {};
    return receive_symbol(FilterInputKind::repair,
        SequenceNumber {packet.control.type_specific},
        packet.payload.subspan(4, size), first, now);
}

Error MemberFilterRuntime::poll_sender(
    SequenceNumber first, std::uint64_t now) noexcept
{
    if (!started_ || failure_ != Error::none)
        return failure_ == Error::none ? Error::invalid_state : failure_;
    if (pending_count_ != 0)
        return Error::none;
    if (advance(sender_, first, now, true) != Error::none)
        return failure_;
    return queue(sender_.instance.poll(now));
}

std::optional<std::span<const std::byte>>
MemberFilterRuntime::pending() const noexcept
{
    return failure_ == Error::none && pending_count_ != 0
        ? std::optional {std::span<const std::byte> {pending_[head_].bytes}
                  .first(pending_[head_].size)}
        : std::nullopt;
}

void MemberFilterRuntime::consume() noexcept
{
    if (pending_count_) {
        pending_[head_].size = 0;
        head_ = (head_ + 1) % maximum_outputs;
        --pending_count_;
    }
}
}
