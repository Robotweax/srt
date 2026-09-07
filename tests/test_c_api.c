#include "robotweax_srt.h"

#include <stdint.h>

int main(void)
{
    const uint8_t packet[16] = {
        0x00, 0x00, 0x00, 0x2a,
        0xc0, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x07,
        0x00, 0x00, 0x00, 0x09,
    };
    robotweax_srt_packet_info info = {0};
    info.struct_size = sizeof(info);
    info.abi_version = ROBOTWEAX_SRT_ABI_VERSION;
    if (robotweax_srt_inspect_packet(packet, sizeof(packet), &info) != ROBOTWEAX_SRT_OK)
        return 1;
    if (info.kind != ROBOTWEAX_SRT_DATA_PACKET || info.sequence_number != 42)
        return 2;
    if (info.timestamp != 7 || info.destination_socket_id != 9)
        return 3;
    if (robotweax_srt_inspect_packet(NULL, sizeof(packet), &info) != ROBOTWEAX_SRT_INVALID_ARGUMENT)
        return 4;
    info.abi_version = ROBOTWEAX_SRT_ABI_VERSION + 1;
    if (robotweax_srt_inspect_packet(packet, sizeof(packet), &info) != ROBOTWEAX_SRT_ABI_MISMATCH)
        return 5;
    robotweax_srt_options* options = NULL;
    if (robotweax_srt_options_create(&options) != ROBOTWEAX_SRT_OK || options == NULL)
        return 6;
    if (robotweax_srt_options_set(options, ROBOTWEAX_SRT_OPT_INPUT_BANDWIDTH, 200000) != ROBOTWEAX_SRT_OK)
        return 7;
    int64_t value = 0;
    if (robotweax_srt_options_get(options, ROBOTWEAX_SRT_OPT_INPUT_BANDWIDTH, &value) != ROBOTWEAX_SRT_OK
        || value != 200000)
        return 8;
    if (robotweax_srt_options_set(options, ROBOTWEAX_SRT_OPT_OVERHEAD_PERCENT, 101)
        != ROBOTWEAX_SRT_INVALID_OPTION_VALUE)
        return 9;
    if (robotweax_srt_options_set(options, ROBOTWEAX_SRT_OPT_RENDEZVOUS, 1)
            != ROBOTWEAX_SRT_OK
        || robotweax_srt_options_get(options, ROBOTWEAX_SRT_OPT_RENDEZVOUS, &value)
            != ROBOTWEAX_SRT_OK
        || value != 1)
        return 10;
    if (robotweax_srt_options_set(options, 99, 1)
        != ROBOTWEAX_SRT_INVALID_OPTION)
        return 11;
    if (robotweax_srt_options_set(options,
            ROBOTWEAX_SRT_OPT_TRANSMISSION_TYPE, 1)
            != ROBOTWEAX_SRT_OK
        || robotweax_srt_options_get(options,
            ROBOTWEAX_SRT_OPT_MESSAGE_API, &value)
            != ROBOTWEAX_SRT_OK
        || value != 0)
        return 12;
    if (robotweax_srt_options_set(options,
            ROBOTWEAX_SRT_OPT_MAXIMUM_REORDER_TOLERANCE, 7)
            != ROBOTWEAX_SRT_OK
        || robotweax_srt_options_get(options,
            ROBOTWEAX_SRT_OPT_MAXIMUM_REORDER_TOLERANCE, &value)
            != ROBOTWEAX_SRT_OK
        || value != 7)
        return 13;
    robotweax_srt_options_destroy(options);
    return 0;
}
