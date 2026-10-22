#include "compiled_filter.hpp"
#include "robotweax/srt/codec.hpp"

namespace robotweax::srt::extensions {

std::span<const CompiledFilter> compiled_filters() noexcept
{
    static const std::array filters {CompiledFilter {.name = "rwx-repeat-v1",
        .implementation = {
            .descriptor = {.scope = FilterScope::member_transport,
                .feedback = FeedbackRequirement::none,
                .state_bytes = 1,
                .maximum_outputs_per_batch = 1,
                .maximum_output_bytes_per_batch = 1472},
            .construct =
                [](std::span<std::byte>, const FilterOwner&, FilterLimits,
                    const FilterEnvironment&) {
                    return Error::none;
                },
            .on_event =
                [](std::span<std::byte>, const FilterEvent& event,
                    FilterOutputWriter& writer) {
                    if (!event.input)
                        return Error::none;
                    const bool sender =
                        event.environment.context.direction == 0;
                    if ((sender && event.input->kind != FilterInputKind::source)
                        || (!sender
                            && event.input->kind != FilterInputKind::repair))
                        return Error::none;
                    if (!sender) {
                        // Opaque repair bytes are untrusted input. Invalid symbols
                        // are dropped rather than turned into bad core candidates.
                        const auto decoded =
                            decode_packet(event.input->payload);
                        if (!decoded || decoded.packet.kind != PacketKind::data
                            || decoded.packet.data.message_number == 0
                            || decoded.packet.data.encryption_key
                                != EncryptionKey::none
                            || decoded.packet.data.destination_socket_id
                                != event.environment.context.session.value
                            || decoded.packet.data.sequence.value()
                                != event.input->member_sequence
                            || decoded.packet.payload.empty())
                            return Error::none;
                    }
                    return writer.append({.context = event.environment.context,
                        .kind = sender ? FilterOutputKind::repair
                                       : FilterOutputKind::recovered_data,
                        .domain = event.input->domain,
                        .sequence = event.input->sequence,
                        .payload = event.input->payload});
                },
            .destroy = [](std::span<std::byte>) noexcept { }}}};
    return filters;
}
}
