/* SPDX-License-Identifier: MIT */
/* Decode the finite elementary-stream fixture without playback clocks or a
 * video-output picture pool. VLC playback is the consumer under test, not
 * the source of the complete expected-frame set. */
#include <stdint.h>
#include <stdio.h>
#include <mpeg2dec/mpeg2.h>

static int frame(const mpeg2_info_t* info)
{
    const mpeg2_sequence_t* s = info->sequence;
    if (s == NULL || s->picture_width != 128 || s->picture_height != 96
        || s->chroma_width != s->width / 2
        || s->chroma_height != s->height / 2) {
        fprintf(stderr, "unexpected reference frame format\n");
        return 1;
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned p = 0; p < 3; ++p) {
        const unsigned width = p == 0 ? s->picture_width : s->picture_width / 2;
        const unsigned height =
            p == 0 ? s->picture_height : s->picture_height / 2;
        const unsigned pitch = p == 0 ? s->width : s->chroma_width;
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                hash ^= info->display_fbuf->buf[p][y * pitch + x];
                hash *= UINT64_C(1099511628211);
            }
        }
    }
    printf("FRAME %016llx\n", (unsigned long long)hash);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    FILE* input = fopen(argv[1], "rb");
    if (input == NULL)
        return 1;
    mpeg2dec_t* decoder = mpeg2_init();
    if (decoder == NULL) {
        fclose(input);
        return 1;
    }
    const mpeg2_info_t* info = mpeg2_info(decoder);
    uint8_t buffer[4096];
    /* Flush the final delayed display picture with a sequence-end code. */
    uint8_t end[] = {0, 0, 1, 0xb7};
    int ended = 0;
    int result = 0;
    for (;;) {
        const mpeg2_state_t state = mpeg2_parse(decoder);
        if (state == STATE_BUFFER) {
            if (ended)
                break;
            size_t n = fread(buffer, 1, sizeof(buffer), input);
            if (ferror(input)) {
                result = 1;
                break;
            }
            if (n != 0)
                mpeg2_buffer(decoder, buffer, buffer + n);
            else {
                mpeg2_buffer(decoder, end, end + sizeof(end));
                ended = 1;
            }
        } else if (state == STATE_INVALID || state == STATE_INVALID_END) {
            fprintf(stderr, "invalid reference MPEG-2 stream\n");
            result = 1;
            break;
        } else if ((state == STATE_SLICE || state == STATE_END)
            && info->display_fbuf != NULL && frame(info) != 0) {
            result = 1;
            break;
        }
    }
    mpeg2_close(decoder);
    fclose(input);
    return result || ferror(stdout);
}
