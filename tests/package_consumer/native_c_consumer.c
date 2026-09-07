#include <robotweax_srt.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    robotweax_srt_options* options = NULL;
    if (robotweax_srt_options_create(&options) != ROBOTWEAX_SRT_OK
        || options == NULL) {
        return 1;
    }

    if (robotweax_srt_options_set(
            options, ROBOTWEAX_SRT_OPT_INPUT_BANDWIDTH, INT64_C(2000000))
        != ROBOTWEAX_SRT_OK) {
        robotweax_srt_options_destroy(options);
        return 2;
    }

    int64_t bandwidth = 0;
    if (robotweax_srt_options_get(
            options, ROBOTWEAX_SRT_OPT_INPUT_BANDWIDTH, &bandwidth)
            != ROBOTWEAX_SRT_OK
        || bandwidth != INT64_C(2000000)) {
        robotweax_srt_options_destroy(options);
        return 3;
    }
    robotweax_srt_options_destroy(options);

    const unsigned char packet[16] = {
        0x00,
        0x00,
        0x00,
        0x01,
        0xC0,
        0x00,
        0x00,
        0x01,
        0x00,
        0x00,
        0x00,
        0x02,
        0x00,
        0x00,
        0x00,
        0x03,
    };
    robotweax_srt_packet_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    info.abi_version = ROBOTWEAX_SRT_ABI_VERSION;
    if (robotweax_srt_inspect_packet(packet, sizeof(packet), &info)
            != ROBOTWEAX_SRT_OK
        || info.kind != ROBOTWEAX_SRT_DATA_PACKET || info.sequence_number != 1U
        || info.payload_offset != sizeof(packet) || info.payload_size != 0U) {
        return 4;
    }

    return 0;
}
