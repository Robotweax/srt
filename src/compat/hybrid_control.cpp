#include "compat/hybrid_control.hpp"

#include <algorithm>

namespace robotweax::srt::compat::hybrid {
namespace {
std::uint32_t get32(const std::byte* p) noexcept
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value = (value << 8) | std::to_integer<unsigned>(p[i]);
    return value;
}
std::uint64_t get64(const std::byte* p) noexcept
{
    return (static_cast<std::uint64_t>(get32(p)) << 32) | get32(p + 4);
}
void put32(std::byte* p, std::uint32_t value) noexcept
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = std::byte(value >> (24 - i * 8));
}
void put64(std::byte* p, std::uint64_t value) noexcept
{
    put32(p, static_cast<std::uint32_t>(value >> 32));
    put32(p + 4, static_cast<std::uint32_t>(value));
}
bool selection_matches(
    const Capability& offer, const Capability& selected) noexcept
{
    return valid(offer) && offer.kind == CapabilityKind::offer
        && valid(selected) && selected.kind == CapabilityKind::selection
        && !(selected.supported & ~offer.supported)
        && (selected.supported & offer.required) == offer.required
        && selected.prefix_limit <= offer.prefix_limit;
}
}
bool valid(const Capability& c) noexcept
{
    if ((c.supported & ~known_features) || (c.required & ~c.supported)
        || (c.supported & core_features) != core_features
        || (c.required & core_features) != core_features || !c.prefix_limit
        || c.prefix_limit > maximum_prefix)
        return false;
    if (c.kind == CapabilityKind::offer)
        return !c.stream && !c.member && !c.initial_position;
    return c.kind == CapabilityKind::selection && c.stream && c.member;
}
bool encode(const Capability& c, std::span<std::byte> out) noexcept
{
    if (!valid(c) || out.size() != capability_size)
        return false;
    std::array<std::byte, capability_size> wire {};
    put32(wire.data(), version);
    put32(wire.data() + 4, static_cast<std::uint32_t>(c.kind));
    put32(wire.data() + 8, c.supported);
    put32(wire.data() + 12, c.required);
    put32(wire.data() + 16, c.prefix_limit);
    put64(wire.data() + 24, c.stream);
    put64(wire.data() + 32, c.member);
    put64(wire.data() + 40, c.initial_position);
    std::copy(wire.begin(), wire.end(), out.begin());
    return true;
}
bool decode(std::span<const std::byte> in, Capability& out) noexcept
{
    out = {.supported = 0, .required = 0, .prefix_limit = 0};
    if (in.size() != capability_size || get32(in.data()) != version
        || get32(in.data() + 20))
        return false;
    Capability c {.kind = static_cast<CapabilityKind>(get32(in.data() + 4)),
        .supported = get32(in.data() + 8),
        .required = get32(in.data() + 12),
        .prefix_limit = get32(in.data() + 16),
        .stream = get64(in.data() + 24),
        .member = get64(in.data() + 32),
        .initial_position = get64(in.data() + 40)};
    if (!valid(c))
        return false;
    out = c;
    return true;
}
bool negotiate(const Capability& peer, const Capability& local,
    std::uint64_t stream, std::uint64_t member, std::uint64_t position,
    Capability& out) noexcept
{
    out = {.supported = 0, .required = 0, .prefix_limit = 0};
    if (!valid(peer) || !valid(local) || peer.kind != CapabilityKind::offer
        || local.kind != CapabilityKind::offer)
        return false;
    const auto selected = peer.supported & local.supported;
    if ((selected & peer.required) != peer.required
        || (selected & local.required) != local.required)
        return false;
    Capability c {.kind = CapabilityKind::selection,
        .supported = selected,
        .required = local.required,
        .prefix_limit = std::min(peer.prefix_limit, local.prefix_limit),
        .stream = stream,
        .member = member,
        .initial_position = position};
    if (!valid(c))
        return false;
    out = c;
    return true;
}
bool valid(const Context& c) noexcept
{
    if (!(valid_multicast_dispatch(c.dispatch)
            && valid_multicast_source(c.source)
            && c.source.kind == MulticastSourceKind::announcement
            && c.source.group_id == c.dispatch.group_id
            && c.source.endpoint.family == c.dispatch.multicast_group.family
            && selection_matches(c.offer, c.selection)))
        return false;
    std::array<std::byte, 64> wire {};
    if (!encode_multicast_dispatch(c.dispatch, wire))
        return false;
    const auto canonical = decode_multicast_dispatch(
        {.type = HandshakeExtensionType::multicast_dispatch,
            .content = std::span {wire}.subspan(4)});
    return canonical && canonical.dispatch == c.dispatch;
}
bool encode(const Context& c, std::span<std::byte> out) noexcept
{
    if (!valid(c) || out.size() != context_size)
        return false;
    std::array<std::byte, context_size> wire {};
    put32(wire.data(), 0x52574801U);
    std::array<std::byte, 64> dispatch {};
    std::array<std::byte, 36> source {};
    if (!encode_multicast_dispatch(c.dispatch, dispatch)
        || !encode_multicast_source(c.source, source)
        || !encode(c.offer, std::span {wire}.subspan(96, 48))
        || !encode(c.selection, std::span {wire}.subspan(144, 48)))
        return false;
    std::copy(dispatch.begin() + 4, dispatch.end(), wire.begin() + 4);
    std::copy(source.begin() + 4, source.end(), wire.begin() + 64);
    std::copy(wire.begin(), wire.end(), out.begin());
    return true;
}
bool decode(std::span<const std::byte> in, Context& out) noexcept
{
    out = {};
    if (in.size() != context_size || get32(in.data()) != 0x52574801U)
        return false;
    const auto dispatch = decode_multicast_dispatch(
        {.type = HandshakeExtensionType::multicast_dispatch,
            .content = in.subspan(4, 60)});
    const auto source = decode_multicast_source(
        {.type = HandshakeExtensionType::multicast_source,
            .content = in.subspan(64, 32)});
    Context c;
    if (!dispatch || !source || !decode(in.subspan(96, 48), c.offer)
        || !decode(in.subspan(144, 48), c.selection))
        return false;
    c.dispatch = dispatch.dispatch;
    c.source = source.source;
    if (!valid(c))
        return false;
    out = c;
    return true;
}
bool sender_is_caller(Kind kind) noexcept
{
    return kind == Kind::path_report || kind == Kind::fallback_request
        || kind == Kind::multicast_ready || kind == Kind::delivery_query;
}
bool valid(const Message& m) noexcept
{
    if (m.kind < Kind::stream_progress || m.kind > Kind::delivery_query
        || !m.stream || !m.member || !m.serial
        || m.next_sequence > SequenceNumber::mask || m.mode > Mode::overlap
        || m.activity > Activity::source_failed
        || m.outcome > Outcome::unsupported
        || m.interval_us > maximum_interval_us || m.age_us > maximum_age_us)
        return false;
    const bool timing = m.kind == Kind::stream_progress
        || m.kind == Kind::path_report || m.kind == Kind::multicast_ready;
    if (timing ? !m.interval_us : (m.interval_us || m.age_us))
        return false;
    if (m.kind != Kind::delivery_status && m.outcome != Outcome::ok)
        return false;
    if (m.kind != Kind::stream_progress && m.activity != Activity::startup)
        return false;
    if (m.request_serial && m.kind != Kind::delivery_commit
        && m.kind != Kind::delivery_status)
        return false;
    if (m.kind == Kind::stream_progress)
        return m.retained_floor <= m.produced_next
            && m.next_position <= m.produced_next
            && m.multicast_next <= m.next_position;
    if (m.produced_next)
        return false;
    if (m.kind == Kind::delivery_commit || m.kind == Kind::delivery_status)
        return !m.multicast_next && m.retained_floor <= m.next_position
            && (m.kind != Kind::delivery_commit || m.revision);
    if (m.retained_floor)
        return false;
    if (m.kind == Kind::fallback_request)
        return m.mode == Mode::unicast && !m.multicast_next;
    if (m.kind == Kind::delivery_query)
        return m.mode == Mode::multicast && !m.multicast_next;
    return m.multicast_next <= m.next_position;
}
bool valid(const Message& m, const Context& c) noexcept
{
    if (!valid(m) || !valid(c) || m.stream != c.selection.stream
        || m.member != c.selection.member
        || m.next_position < c.selection.initial_position)
        return false;
    const auto sequence =
        (c.dispatch.initial_sequence
            + ((m.next_position - c.selection.initial_position)
                & SequenceNumber::mask))
        & SequenceNumber::mask;
    if (m.next_sequence != sequence)
        return false;
    if ((m.kind == Kind::delivery_query || m.request_serial)
        && !(c.selection.supported & policy_query))
        return false;
    if ((m.mode == Mode::overlap || m.kind == Kind::multicast_ready)
        && !(c.selection.supported & overlap))
        return false;
    if (m.kind == Kind::stream_progress
        && (m.produced_next < c.selection.initial_position
            || m.retained_floor < c.selection.initial_position
            || m.multicast_next < c.selection.initial_position))
        return false;
    if ((m.kind == Kind::path_report || m.kind == Kind::multicast_ready)
        && m.multicast_next < c.selection.initial_position)
        return false;
    if ((m.kind == Kind::delivery_commit || m.kind == Kind::delivery_status)
        && m.retained_floor < c.selection.initial_position)
        return false;
    return true;
}
bool encode(const Message& m, std::span<std::byte> out) noexcept
{
    if (!valid(m) || out.size() != message_size)
        return false;
    std::array<std::byte, message_size> wire {};
    put32(wire.data(), version);
    put32(wire.data() + 4, static_cast<std::uint32_t>(m.kind));
    put64(wire.data() + 8, m.stream);
    put64(wire.data() + 16, m.member);
    put64(wire.data() + 24, m.serial);
    put64(wire.data() + 32, m.revision);
    put64(wire.data() + 40, m.next_position);
    put32(wire.data() + 48, m.next_sequence);
    put32(wire.data() + 52, static_cast<std::uint32_t>(m.mode));
    put64(wire.data() + 56, m.retained_floor);
    put64(wire.data() + 64, m.multicast_next);
    put64(wire.data() + 72,
        (m.kind == Kind::delivery_commit || m.kind == Kind::delivery_status)
            ? m.request_serial
            : m.produced_next);
    put32(wire.data() + 80, static_cast<std::uint32_t>(m.activity));
    put32(wire.data() + 84, static_cast<std::uint32_t>(m.outcome));
    put32(wire.data() + 88, m.interval_us);
    put32(wire.data() + 92, m.age_us);
    std::copy(wire.begin(), wire.end(), out.begin());
    return true;
}
bool decode(std::span<const std::byte> in, Message& out) noexcept
{
    out = {};
    if (in.size() != message_size || get32(in.data()) != version)
        return false;
    Message m {.kind = static_cast<Kind>(get32(in.data() + 4)),
        .stream = get64(in.data() + 8),
        .member = get64(in.data() + 16),
        .serial = get64(in.data() + 24),
        .revision = get64(in.data() + 32),
        .next_position = get64(in.data() + 40),
        .next_sequence = get32(in.data() + 48),
        .mode = static_cast<Mode>(get32(in.data() + 52)),
        .retained_floor = get64(in.data() + 56),
        .multicast_next = get64(in.data() + 64),
        .produced_next = 0,
        .activity = static_cast<Activity>(get32(in.data() + 80)),
        .outcome = static_cast<Outcome>(get32(in.data() + 84)),
        .interval_us = get32(in.data() + 88),
        .age_us = get32(in.data() + 92)};
    if (m.kind == Kind::delivery_commit || m.kind == Kind::delivery_status)
        m.request_serial = get64(in.data() + 72);
    else
        m.produced_next = get64(in.data() + 72);
    if (!valid(m))
        return false;
    out = m;
    return true;
}

#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
Session::Session(
    SessionAuthentication& auth, const Context& context, bool caller) noexcept
    : auth_(auth)
    , context_(context)
    , caller_(caller)
{
    claimed_ = encode(context, binding_)
        && auth_.claim_hybrid_control(caller_, binding_);
}
bool Session::ready() const noexcept
{
    return claimed_ && !closed_
        && auth_.hybrid_control_ready(caller_, binding_);
}
Session::Status Session::Window::classify(
    std::uint64_t serial, std::span<const std::byte> wire) const noexcept
{
    if (!serial || wire.size() != message_size)
        return Status::invalid;
    if (serial <= highest && highest - serial >= entries.size())
        return Status::stale;
    const auto& entry = entries[serial % entries.size()];
    if (entry.serial != serial)
        return Status::accepted;
    return std::equal(wire.begin(), wire.end(), entry.wire.begin())
        ? Status::duplicate
        : Status::conflict;
}
void Session::Window::commit(std::uint64_t serial,
    std::span<const std::byte> wire, std::uint64_t at) noexcept
{
    auto& entry = entries[serial % entries.size()];
    entry.serial = serial;
    entry.at = at;
    std::copy(wire.begin(), wire.end(), entry.wire.begin());
    highest = std::max(highest, serial);
}
Session::Status Session::seal(const Message& m, PacketTimestamp timestamp,
    std::span<std::byte> out, std::size_t& written) noexcept
{
    written = 0;
    if (closed_)
        return Status::closed;
    if (!ready() || !valid(m, context_) || sender_is_caller(m.kind) != caller_)
        return Status::invalid;
    std::array<std::byte, message_size> body {};
    if (!encode(m, body))
        return Status::invalid;
    const auto status = sent_.classify(m.serial, body);
    if (status != Status::accepted && status != Status::duplicate)
        return status;
    std::array<std::byte, packet_header_size + message_size> packet {};
    if (!encode_packet(
            {.kind = PacketKind::control,
                .control = {.type = ControlType::user_defined,
                    .subtype = control_subtype,
                    .timestamp = timestamp,
                    .destination_socket_id = auth_.hybrid_peer_socket_id()},
                .payload = body},
            packet)
        || auth_.seal_control(packet, out, written) != Error::none)
        return Status::authentication_failed;
    if (status == Status::accepted)
        sent_.commit(m.serial, body, 0);
    return status;
}
Session::Result Session::receive(
    const PacketView& envelope, std::uint64_t now) noexcept
{
    if (closed_)
        return {.status = Status::closed};
    if (!ready() || now < last_now_)
        return {};
    if (envelope.control.destination_socket_id
        != auth_.hybrid_local_socket_id())
        return {.status = Status::authentication_failed};
    PacketView packet;
    if (!auth_.open_control(envelope, packet))
        return {.status = Status::authentication_failed};
    Message m;
    if (!decode(packet.payload, m) || !valid(m, context_)
        || sender_is_caller(m.kind) == caller_)
        return {};
    const auto status = received_.classify(m.serial, packet.payload);
    if (status != Status::accepted && status != Status::duplicate)
        return {.status = status};
    const bool report =
        m.kind == Kind::stream_progress || m.kind == Kind::path_report;
    const auto index = m.kind == Kind::stream_progress ? 0U : 1U;
    if (status == Status::accepted && report
        && m.serial < reports_[index].serial)
        return {.status = Status::stale};
    last_now_ = now;
    if (status == Status::accepted) {
        received_.commit(m.serial, packet.payload, now);
        if (report)
            reports_[index] = {m.serial, now, m.age_us};
    }
    return {.status = status,
        .message = m,
        .first_received_us = received_.entries[m.serial % 64].at};
}
bool Session::fresh(
    Kind kind, std::uint64_t now, std::uint32_t limit) const noexcept
{
    if (!ready() || limit > maximum_age_us
        || (kind != Kind::stream_progress && kind != Kind::path_report))
        return false;
    const auto& report = reports_[kind == Kind::stream_progress ? 0U : 1U];
    return report.serial && now >= report.at && limit >= report.age
        && now - report.at <= limit - report.age;
}
void Session::close() noexcept
{
    closed_ = true;
    sent_ = {};
    received_ = {};
    reports_ = {};
}
#endif
} // namespace robotweax::srt::compat::hybrid
