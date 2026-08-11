#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdio_frame_protocol.h"

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            fprintf(stderr, "layout check failed at line %d: %s\n",             \
                    __LINE__, #expression);                                      \
            return 1;                                                            \
        }                                                                        \
    } while (0)

int main(void)
{
    CHECK(SDIO_FRAME_MSG_ID == UINT32_C(0x504a5047));
    CHECK(SDIO_FRAME_CONTROL_MSG_ID == UINT32_C(0x5043544c));
    CHECK(SDIO_FRAME_MAGIC == UINT32_C(0x46544a50));
    CHECK(SDIO_FRAME_CONTROL_MAGIC == UINT32_C(0x4354524c));
    CHECK(SDIO_FRAME_VERSION == 3);
    CHECK(SDIO_FRAME_CHUNK_DATA_MAX == 7600);
    CHECK(SDIO_FRAME_MAX_JPEG_SIZE == 30720);

    CHECK(SDIO_FRAME_CONTROL_QUERY == 1);
    CHECK(SDIO_FRAME_CONTROL_NOT_READY == 2);
    CHECK(SDIO_FRAME_CONTROL_READY == 3);
    CHECK(SDIO_FRAME_CONTROL_BUSY == 4);
    CHECK(SDIO_FRAME_CONTROL_ACCEPTED == 5);
    CHECK(SDIO_FRAME_CONTROL_SERVER_ACKED == 6);
    CHECK(SDIO_FRAME_CONTROL_FAILED == 7);
    CHECK(SDIO_FRAME_REASON_INTERNAL == 11);

    CHECK(sizeof(sdio_frame_chunk_header_t) == 44);
    CHECK(offsetof(sdio_frame_chunk_header_t, magic) == 0);
    CHECK(offsetof(sdio_frame_chunk_header_t, version) == 4);
    CHECK(offsetof(sdio_frame_chunk_header_t, header_size) == 6);
    CHECK(offsetof(sdio_frame_chunk_header_t, frame_id) == 8);
    CHECK(offsetof(sdio_frame_chunk_header_t, chunk_index) == 12);
    CHECK(offsetof(sdio_frame_chunk_header_t, chunk_count) == 14);
    CHECK(offsetof(sdio_frame_chunk_header_t, flags) == 16);
    CHECK(offsetof(sdio_frame_chunk_header_t, chunk_offset) == 20);
    CHECK(offsetof(sdio_frame_chunk_header_t, chunk_size) == 24);
    CHECK(offsetof(sdio_frame_chunk_header_t, jpeg_size) == 28);
    CHECK(offsetof(sdio_frame_chunk_header_t, detected_at_ms) == 32);
    CHECK(offsetof(sdio_frame_chunk_header_t, jpeg_crc32) == 40);

    CHECK(sizeof(sdio_frame_control_t) == 24);
    CHECK(offsetof(sdio_frame_control_t, state) == 8);
    CHECK(offsetof(sdio_frame_control_t, frame_id) == 12);
    CHECK(offsetof(sdio_frame_control_t, ble_frame_id) == 16);
    CHECK(offsetof(sdio_frame_control_t, reason) == 18);
    CHECK(offsetof(sdio_frame_control_t, detail) == 20);

    const sdio_frame_control_t control = {
        .magic = SDIO_FRAME_CONTROL_MAGIC,
        .version = SDIO_FRAME_VERSION,
        .size = sizeof(sdio_frame_control_t),
        .state = SDIO_FRAME_CONTROL_ACCEPTED,
        .reserved = 0,
        .frame_id = UINT32_C(0x12345678),
        .ble_frame_id = UINT16_C(0x9abc),
        .reason = SDIO_FRAME_REASON_NONE,
        .detail = UINT32_C(0x01020304),
    };
    const uint8_t expected[24] = {
        0x4c, 0x52, 0x54, 0x43, 0x03, 0x00, 0x18, 0x00,
        0x05, 0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12,
        0xbc, 0x9a, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01,
    };
    CHECK(memcmp(&control, expected, sizeof(expected)) == 0);

    puts("protocol layout: PASS");
    return 0;
}
