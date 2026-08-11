#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_crc.h"
#include "image_reassembly.h"

#define SOURCE_A UINT16_C(0x0002)
#define SOURCE_B UINT16_C(0x0003)
#define FRAME_A  UINT16_C(0x1234)

#define EXPECT(condition) do {                                             \
    if (!(condition)) {                                                    \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
        return false;                                                      \
    }                                                                      \
} while (0)

static size_t make_jpeg(uint8_t *jpeg, size_t size,
                        uint16_t width, uint16_t height)
{
    memset(jpeg, 0x5a, size);
    const uint8_t prefix[] = {
        0xff, 0xd8,
        0xff, 0xc0,
        0x00, 0x11,
        0x08,
        (uint8_t)(height >> 8), (uint8_t)height,
        (uint8_t)(width >> 8), (uint8_t)width,
        0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
    };
    memcpy(jpeg, prefix, sizeof(prefix));
    jpeg[size - 2U] = 0xff;
    jpeg[size - 1U] = 0xd9;
    return size;
}

static void encode_open(uint8_t wire[16], uint16_t frame_id,
                        uint64_t detected_at_ms, uint16_t image_len,
                        uint32_t crc)
{
    ble_mesh_image_put_le16(wire, frame_id);
    ble_mesh_image_put_le64(wire + 2, detected_at_ms);
    ble_mesh_image_put_le16(wire + 10, image_len);
    ble_mesh_image_put_le32(wire + 12, crc);
}

static size_t feed(image_reassembly_t *state, uint16_t source,
                   uint8_t opcode, const uint8_t *wire, size_t wire_len,
                   uint64_t now_ms, image_reassembly_reply_t replies[3],
                   image_reassembly_complete_t *complete)
{
    memset(replies, 0, sizeof(*replies) * 3U);
    if (complete != NULL) {
        memset(complete, 0, sizeof(*complete));
    }
    return image_reassembly_receive(state, source, opcode, wire, wire_len,
                                    now_ms, replies, 3U, complete);
}

static size_t feed_chunk(image_reassembly_t *state, uint16_t source,
                         uint16_t frame_id, uint8_t index,
                         const uint8_t *jpeg, size_t jpeg_len,
                         uint64_t now_ms, image_reassembly_reply_t replies[3])
{
    uint8_t wire[sizeof(ble_mesh_image_data_header_t) +
                 BLE_MESH_IMAGE_DATA_BYTES];
    size_t offset = (size_t)index * BLE_MESH_IMAGE_DATA_BYTES;
    size_t data_len = jpeg_len - offset;
    if (data_len > BLE_MESH_IMAGE_DATA_BYTES) {
        data_len = BLE_MESH_IMAGE_DATA_BYTES;
    }
    ble_mesh_image_put_le16(wire, frame_id);
    wire[2] = index;
    memcpy(wire + 3, jpeg + offset, data_len);
    return feed(state, source, BLE_MESH_IMAGE_OP_DATA, wire,
                3U + data_len, now_ms, replies, NULL);
}

static bool test_protocol_layout_and_endian(void)
{
    EXPECT(sizeof(ble_mesh_image_open_t) == 16U);
    EXPECT(sizeof(ble_mesh_image_data_header_t) == 3U);
    EXPECT(BLE_MESH_IMAGE_DATA_WIRE_MAX == 377U);
    EXPECT(BLE_MESH_IMAGE_MAX_CHUNKS == 83U);
    uint8_t wire[8];
    ble_mesh_image_put_le64(wire, UINT64_C(0x0123456789abcdef));
    EXPECT(ble_mesh_image_get_le64(wire) == UINT64_C(0x0123456789abcdef));
    static const uint8_t check[] = "123456789";
    EXPECT(esp_crc32_le(0, check, 9U) == UINT32_C(0xcbf43926));
    return true;
}

static bool test_open_size_boundaries(void)
{
    const uint16_t accepted[] = {1U, 374U, 375U, 30720U};
    for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
        image_reassembly_t state;
        image_reassembly_init(&state);
        uint8_t open[16];
        encode_open(open, FRAME_A, 1U, accepted[i], 0U);
        image_reassembly_reply_t replies[3];
        EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                    open, sizeof(open), 1U, replies, NULL) == 1U);
        EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_ACCEPT);
    }

    image_reassembly_t state;
    image_reassembly_init(&state);
    uint8_t too_large[16];
    encode_open(too_large, FRAME_A, 1U, 30721U, 0U);
    image_reassembly_reply_t replies[3];
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                too_large, sizeof(too_large), 1U, replies, NULL) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_REJECT);
    EXPECT(replies[0].payload[2] == BLE_MESH_IMAGE_REJECT_SIZE);
    return true;
}

static bool test_out_of_order_nack_complete_and_cache(void)
{
    image_reassembly_t state;
    image_reassembly_init(&state);
    uint8_t jpeg[900];
    make_jpeg(jpeg, sizeof(jpeg), 224U, 224U);
    uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    uint8_t open[16];
    encode_open(open, FRAME_A, UINT64_C(1780000000123), sizeof(jpeg), crc);
    image_reassembly_reply_t replies[3];
    image_reassembly_complete_t complete;

    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                open, sizeof(open), 100U, replies, &complete) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_ACCEPT);
    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 2U, jpeg, sizeof(jpeg),
                      110U, replies) == 0U);
    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 0U, jpeg, sizeof(jpeg),
                      120U, replies) == 0U);

    uint8_t end[2];
    ble_mesh_image_put_le16(end, FRAME_A);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 130U, replies, &complete) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_NACK);
    EXPECT(replies[0].payload[2] == 0U);
    EXPECT((replies[0].payload[3] & (1U << 1)) != 0U);

    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 1U, jpeg, sizeof(jpeg),
                      140U, replies) == 0U);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 150U, replies, &complete) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_COMPLETE);
    EXPECT(complete.valid && complete.source_addr == SOURCE_A);
    EXPECT(complete.frame_id == FRAME_A);
    EXPECT(complete.detected_at_ms == UINT64_C(1780000000123));
    EXPECT(complete.jpeg_len == sizeof(jpeg));
    EXPECT(memcmp(complete.jpeg, jpeg, sizeof(jpeg)) == 0);

    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 160U, replies, &complete) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_COMPLETE);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 150U + IMAGE_REASSEMBLY_CACHE_MS - 1U,
                replies, &complete) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_COMPLETE);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 150U + IMAGE_REASSEMBLY_CACHE_MS,
                replies, &complete) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_RESTART);
    return true;
}

static bool test_busy_duplicate_and_restart(void)
{
    image_reassembly_t state;
    image_reassembly_init(&state);
    uint8_t jpeg[400];
    make_jpeg(jpeg, sizeof(jpeg), 224U, 224U);
    uint8_t open[16];
    encode_open(open, FRAME_A, 0U, sizeof(jpeg),
                esp_crc32_le(0, jpeg, sizeof(jpeg)));
    image_reassembly_reply_t replies[3];

    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                open, sizeof(open), 1U, replies, NULL) == 1U);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                open, sizeof(open), 2U, replies, NULL) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_ACCEPT);

    uint8_t other[16];
    encode_open(other, FRAME_A, 0U, sizeof(jpeg),
                esp_crc32_le(0, jpeg, sizeof(jpeg)));
    EXPECT(feed(&state, SOURCE_B, BLE_MESH_IMAGE_OP_OPEN,
                other, sizeof(other), 3U, replies, NULL) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_BUSY);

    uint8_t data[4] = {0};
    ble_mesh_image_put_le16(data, 99U);
    EXPECT(feed(&state, SOURCE_B, BLE_MESH_IMAGE_OP_DATA,
                data, sizeof(data), 4U, replies, NULL) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_RESTART);
    return true;
}

static bool test_completion_cache_includes_timestamp(void)
{
    image_reassembly_t state;
    image_reassembly_init(&state);
    uint8_t jpeg[200];
    make_jpeg(jpeg, sizeof(jpeg), 224U, 224U);
    uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    uint8_t open[16];
    encode_open(open, FRAME_A, 1000U, sizeof(jpeg), crc);
    image_reassembly_reply_t replies[3];
    image_reassembly_complete_t complete;
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                open, sizeof(open), 1U, replies, NULL) == 1U);
    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 0U, jpeg, sizeof(jpeg),
                      2U, replies) == 0U);
    uint8_t end[2];
    ble_mesh_image_put_le16(end, FRAME_A);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 3U, replies, &complete) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_COMPLETE);

    /* C6 reboot can reuse frame ID and JPEG while event time is new. */
    encode_open(open, FRAME_A, 2000U, sizeof(jpeg), crc);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                open, sizeof(open), 4U, replies, NULL) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_ACCEPT);
    EXPECT(state.active && state.detected_at_ms == 2000U);
    return true;
}

static bool test_duplicate_data_and_restart_after_server_reset(void)
{
    image_reassembly_t state;
    image_reassembly_init(&state);
    uint8_t jpeg[400];
    make_jpeg(jpeg, sizeof(jpeg), 224U, 224U);
    uint8_t open[16];
    encode_open(open, FRAME_A, 1U, sizeof(jpeg),
                esp_crc32_le(0, jpeg, sizeof(jpeg)));
    image_reassembly_reply_t replies[3];
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                open, sizeof(open), 1U, replies, NULL) == 1U);
    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 0U, jpeg, sizeof(jpeg),
                      2U, replies) == 0U);
    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 0U, jpeg, sizeof(jpeg),
                      3U, replies) == 0U);

    uint8_t corrupted[400];
    memcpy(corrupted, jpeg, sizeof(corrupted));
    corrupted[30] ^= 1U;
    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 0U,
                      corrupted, sizeof(corrupted), 4U, replies) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_REJECT);
    EXPECT(replies[0].payload[2] == BLE_MESH_IMAGE_REJECT_STATE);

    image_reassembly_init(&state); /* Simulates a server restart. */
    uint8_t end[2];
    ble_mesh_image_put_le16(end, FRAME_A);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 5U, replies, NULL) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_RESTART);
    uint8_t data[4] = {0};
    ble_mesh_image_put_le16(data, FRAME_A);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_DATA,
                data, sizeof(data), 6U, replies, NULL) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_RESTART);
    return true;
}

static bool finish_invalid(uint16_t width, uint16_t height, bool bad_crc,
                           uint8_t expected_reason)
{
    image_reassembly_t state;
    image_reassembly_init(&state);
    uint8_t jpeg[200];
    make_jpeg(jpeg, sizeof(jpeg), width, height);
    uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    if (bad_crc) {
        crc ^= 1U;
    }
    uint8_t open[16];
    encode_open(open, FRAME_A, 1U, sizeof(jpeg), crc);
    image_reassembly_reply_t replies[3];
    image_reassembly_complete_t complete;
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                open, sizeof(open), 1U, replies, NULL) == 1U);
    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 0U, jpeg, sizeof(jpeg),
                      2U, replies) == 0U);
    uint8_t end[2];
    ble_mesh_image_put_le16(end, FRAME_A);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 3U, replies, &complete) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_REJECT);
    EXPECT(replies[0].payload[2] == expected_reason);
    EXPECT(!complete.valid);
    return true;
}

static bool test_crc_and_dimension_validation(void)
{
    EXPECT(finish_invalid(224U, 224U, true,
                          BLE_MESH_IMAGE_REJECT_CRC));
    EXPECT(finish_invalid(223U, 224U, false,
                          BLE_MESH_IMAGE_REJECT_JPEG));
    return true;
}

static bool finish_bad_marker(unsigned int kind)
{
    image_reassembly_t state;
    image_reassembly_init(&state);
    uint8_t jpeg[200];
    make_jpeg(jpeg, sizeof(jpeg), 224U, 224U);
    if (kind == 0U) {
        jpeg[0] = 0U;
    } else if (kind == 1U) {
        jpeg[sizeof(jpeg) - 1U] = 0U;
    } else {
        jpeg[3] = 0xc4U; /* Remove the only SOF marker. */
    }
    uint8_t open[16];
    encode_open(open, FRAME_A, 1U, sizeof(jpeg),
                esp_crc32_le(0, jpeg, sizeof(jpeg)));
    image_reassembly_reply_t replies[3];
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                open, sizeof(open), 1U, replies, NULL) == 1U);
    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 0U, jpeg, sizeof(jpeg),
                      2U, replies) == 0U);
    uint8_t end[2];
    ble_mesh_image_put_le16(end, FRAME_A);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 3U, replies, NULL) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_REJECT);
    EXPECT(replies[0].payload[2] == BLE_MESH_IMAGE_REJECT_JPEG);
    return true;
}

static bool test_soi_eoi_and_sof_validation(void)
{
    EXPECT(finish_bad_marker(0U));
    EXPECT(finish_bad_marker(1U));
    EXPECT(finish_bad_marker(2U));
    return true;
}

static bool test_first_middle_last_missing_bitmap(void)
{
    image_reassembly_t state;
    image_reassembly_init(&state);
    static uint8_t jpeg[BLE_MESH_IMAGE_MAX_BYTES];
    make_jpeg(jpeg, sizeof(jpeg), 224U, 224U);
    uint8_t open[16];
    encode_open(open, FRAME_A, 1U, sizeof(jpeg),
                esp_crc32_le(0, jpeg, sizeof(jpeg)));
    image_reassembly_reply_t replies[3];
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                open, sizeof(open), 1U, replies, NULL) == 1U);
    for (uint8_t i = 0; i < BLE_MESH_IMAGE_MAX_CHUNKS; ++i) {
        if (i == 0U || i == 41U || i == 82U) {
            continue;
        }
        EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, i,
                          jpeg, sizeof(jpeg), 2U + i, replies) == 0U);
    }
    uint8_t end[2];
    ble_mesh_image_put_le16(end, FRAME_A);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 100U, replies, NULL) == 3U);
    EXPECT(replies[0].payload[2] == 0U &&
           (replies[0].payload[3] & 0x01U) != 0U);
    EXPECT(replies[1].payload[2] == 40U &&
           (replies[1].payload[3] & 0x02U) != 0U);
    EXPECT(replies[2].payload[2] == 80U &&
           (replies[2].payload[3] & 0x04U) != 0U);
    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 0U,
                      jpeg, sizeof(jpeg), 101U, replies) == 0U);
    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 41U,
                      jpeg, sizeof(jpeg), 102U, replies) == 0U);
    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 82U,
                      jpeg, sizeof(jpeg), 103U, replies) == 0U);
    image_reassembly_complete_t complete;
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 104U, replies, &complete) == 1U);
    EXPECT(replies[0].opcode == BLE_MESH_IMAGE_OP_COMPLETE);
    EXPECT(complete.valid && complete.jpeg_len == sizeof(jpeg));
    EXPECT(memcmp(complete.jpeg, jpeg, sizeof(jpeg)) == 0);
    return true;
}

static bool test_timeout_and_maximum_bitmap(void)
{
    image_reassembly_t state;
    image_reassembly_init(&state);
    static uint8_t jpeg[BLE_MESH_IMAGE_MAX_BYTES];
    make_jpeg(jpeg, sizeof(jpeg), 224U, 224U);
    uint8_t open[16];
    encode_open(open, FRAME_A, 0U, sizeof(jpeg),
                esp_crc32_le(0, jpeg, sizeof(jpeg)));
    image_reassembly_reply_t replies[3];
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                open, sizeof(open), 100U, replies, NULL) == 1U);
    uint8_t end[2];
    ble_mesh_image_put_le16(end, FRAME_A);
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 101U, replies, NULL) == 3U);
    EXPECT(replies[0].payload[2] == 0U);
    EXPECT(replies[1].payload[2] == 40U);
    EXPECT(replies[2].payload[2] == 80U);
    EXPECT(replies[2].payload_len == 4U);

    image_reassembly_reply_t timeout;
    EXPECT(image_reassembly_poll_timeout(
        &state, 101U + IMAGE_REASSEMBLY_TIMEOUT_MS - 1U,
        &timeout) == 0U);
    EXPECT(image_reassembly_poll_timeout(
        &state, 101U + IMAGE_REASSEMBLY_TIMEOUT_MS,
        &timeout) == 1U);
    EXPECT(timeout.opcode == BLE_MESH_IMAGE_OP_REJECT);
    EXPECT(timeout.payload[2] == BLE_MESH_IMAGE_REJECT_TIMEOUT);
    return true;
}

static bool test_open_receive_estimate_is_stable(void)
{
    image_reassembly_t state;
    image_reassembly_init(&state);
    uint8_t jpeg[200];
    make_jpeg(jpeg, sizeof(jpeg), 224U, 224U);
    uint8_t open[16];
    encode_open(open, FRAME_A, 0U, sizeof(jpeg),
                esp_crc32_le(0, jpeg, sizeof(jpeg)));
    image_reassembly_reply_t replies[3];
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_OPEN,
                open, sizeof(open), 1U, replies, NULL) == 1U);
    image_reassembly_set_rx_estimate(&state, SOURCE_A, FRAME_A, 1234567U);
    image_reassembly_set_rx_estimate(&state, SOURCE_A, FRAME_A, 7654321U);
    image_reassembly_set_rx_estimate(&state, SOURCE_B, FRAME_A, 9999999U);

    EXPECT(feed_chunk(&state, SOURCE_A, FRAME_A, 0U, jpeg, sizeof(jpeg),
                      2U, replies) == 0U);
    uint8_t end[2];
    ble_mesh_image_put_le16(end, FRAME_A);
    image_reassembly_complete_t complete;
    EXPECT(feed(&state, SOURCE_A, BLE_MESH_IMAGE_OP_END,
                end, sizeof(end), 3U, replies, &complete) == 1U);
    EXPECT(complete.valid);
    EXPECT(complete.detected_at_ms == 0U);
    EXPECT(complete.rx_estimate_ms == 1234567U);
    return true;
}

int main(void)
{
    const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"protocol layout / LE / CRC", test_protocol_layout_and_endian},
        {"1/374/375/30720/30721 OPEN boundaries",
         test_open_size_boundaries},
        {"out-of-order / NACK / completion cache",
         test_out_of_order_nack_complete_and_cache},
        {"busy / duplicate / restart", test_busy_duplicate_and_restart},
        {"completion cache timestamp identity",
         test_completion_cache_includes_timestamp},
        {"duplicate DATA conflict / server restart",
         test_duplicate_data_and_restart_after_server_reset},
        {"CRC / 224x224 validation", test_crc_and_dimension_validation},
        {"SOI / EOI / SOF validation", test_soi_eoi_and_sof_validation},
        {"first / middle / last missing bitmap",
         test_first_middle_last_missing_bitmap},
        {"30 KiB bitmap / timeout", test_timeout_and_maximum_bitmap},
        {"OPEN receive-time estimate is stable",
         test_open_receive_estimate_is_stable},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (!tests[i].run()) {
            printf("FAILED: %s\n", tests[i].name);
            return 1;
        }
        printf("PASS: %s\n", tests[i].name);
    }
    return 0;
}
