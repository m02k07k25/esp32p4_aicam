#ifndef BLE_MESH_IMAGE_PROTOCOL_H
#define BLE_MESH_IMAGE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Protocol generation 2 deliberately has no per-packet magic or version.
 * The vendor model ID is the protocol discriminator. All integer fields are
 * little-endian; every currently supported ESP-IDF target is little-endian.
 */
#define BLE_MESH_IMAGE_COMPANY_ID          UINT16_C(0x02E5)
#define BLE_MESH_IMAGE_SOURCE_MODEL_ID     UINT16_C(0x0002)
#define BLE_MESH_IMAGE_GATEWAY_MODEL_ID    BLE_MESH_IMAGE_SOURCE_MODEL_ID

#define BLE_MESH_IMAGE_MAX_BYTES           UINT16_C(30720)
#define BLE_MESH_IMAGE_DATA_BYTES          UINT16_C(374)
#define BLE_MESH_IMAGE_MAX_CHUNKS          \
    ((BLE_MESH_IMAGE_MAX_BYTES + BLE_MESH_IMAGE_DATA_BYTES - 1U) / \
     BLE_MESH_IMAGE_DATA_BYTES)
#define BLE_MESH_IMAGE_NACK_BITMAP_MAX     5U
#define BLE_MESH_IMAGE_NACK_BITS_MAX       \
    (BLE_MESH_IMAGE_NACK_BITMAP_MAX * 8U)
#define BLE_MESH_IMAGE_DATA_WIRE_MAX       \
    (sizeof(ble_mesh_image_data_header_t) + BLE_MESH_IMAGE_DATA_BYTES)

/* Vendor opcode octets. Wrap with ESP_BLE_MESH_MODEL_OP_3(op, company_id). */
#define BLE_MESH_IMAGE_OP_OPEN             0xC1U
#define BLE_MESH_IMAGE_OP_DATA             0xC2U
#define BLE_MESH_IMAGE_OP_END              0xC3U
#define BLE_MESH_IMAGE_OP_ACCEPT           0xC4U
#define BLE_MESH_IMAGE_OP_BUSY             0xC5U
#define BLE_MESH_IMAGE_OP_COMPLETE         0xC6U
#define BLE_MESH_IMAGE_OP_NACK             0xC7U
#define BLE_MESH_IMAGE_OP_RESTART          0xC8U
#define BLE_MESH_IMAGE_OP_REJECT           0xC9U

typedef enum {
    BLE_MESH_IMAGE_REJECT_MALFORMED = 1,
    BLE_MESH_IMAGE_REJECT_SIZE = 2,
    BLE_MESH_IMAGE_REJECT_STATE = 3,
    BLE_MESH_IMAGE_REJECT_CRC = 4,
    BLE_MESH_IMAGE_REJECT_JPEG = 5,
    BLE_MESH_IMAGE_REJECT_TIMEOUT = 6,
    BLE_MESH_IMAGE_REJECT_INTERNAL = 7,
} ble_mesh_image_reject_reason_t;

/* C1 OPEN: exactly 16 bytes. */
typedef struct __attribute__((packed)) {
    uint16_t frame_id;
    uint64_t detected_at_ms;
    uint16_t image_len;
    uint32_t jpeg_crc32;
} ble_mesh_image_open_t;

/* C2 DATA: this 3-byte prefix followed by 1..374 JPEG bytes. */
typedef struct __attribute__((packed)) {
    uint16_t frame_id;
    uint8_t chunk_index;
} ble_mesh_image_data_header_t;

/* C3 END, C4 ACCEPT, C5 BUSY, C6 COMPLETE, C8 RESTART: exactly 2 bytes. */
typedef struct __attribute__((packed)) {
    uint16_t frame_id;
} ble_mesh_image_frame_t;

/* C7 NACK: this 3-byte prefix followed by 1..5 missing-bit bytes. */
typedef struct __attribute__((packed)) {
    uint16_t frame_id;
    uint8_t base_index;
} ble_mesh_image_nack_header_t;

/* C9 REJECT: exactly 3 bytes. */
typedef struct __attribute__((packed)) {
    uint16_t frame_id;
    uint8_t reason;
} ble_mesh_image_reject_t;

/* Unaligned-safe little-endian codecs for radio callback buffers. */
static inline uint16_t ble_mesh_image_get_le16(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static inline uint32_t ble_mesh_image_get_le32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static inline uint64_t ble_mesh_image_get_le64(const uint8_t *value)
{
    return (uint64_t)ble_mesh_image_get_le32(value) |
           ((uint64_t)ble_mesh_image_get_le32(value + 4) << 32);
}

static inline void ble_mesh_image_put_le16(uint8_t *value, uint16_t input)
{
    value[0] = (uint8_t)input;
    value[1] = (uint8_t)(input >> 8);
}

static inline void ble_mesh_image_put_le32(uint8_t *value, uint32_t input)
{
    value[0] = (uint8_t)input;
    value[1] = (uint8_t)(input >> 8);
    value[2] = (uint8_t)(input >> 16);
    value[3] = (uint8_t)(input >> 24);
}

static inline void ble_mesh_image_put_le64(uint8_t *value, uint64_t input)
{
    ble_mesh_image_put_le32(value, (uint32_t)input);
    ble_mesh_image_put_le32(value + 4, (uint32_t)(input >> 32));
}

#if defined(__cplusplus)
static_assert(sizeof(ble_mesh_image_open_t) == 16U,
              "BLE image OPEN wire layout changed");
static_assert(sizeof(ble_mesh_image_data_header_t) == 3U,
              "BLE image DATA prefix changed");
static_assert(sizeof(ble_mesh_image_frame_t) == 2U,
              "BLE image control wire layout changed");
static_assert(sizeof(ble_mesh_image_nack_header_t) == 3U,
              "BLE image NACK prefix changed");
static_assert(sizeof(ble_mesh_image_reject_t) == 3U,
              "BLE image REJECT wire layout changed");
static_assert(BLE_MESH_IMAGE_MAX_CHUNKS == 83U,
              "BLE image chunk limit changed");
static_assert(sizeof(ble_mesh_image_data_header_t) +
                  BLE_MESH_IMAGE_DATA_BYTES == 377U,
              "BLE image DATA maximum changed");
#else
_Static_assert(sizeof(ble_mesh_image_open_t) == 16U,
               "BLE image OPEN wire layout changed");
_Static_assert(sizeof(ble_mesh_image_data_header_t) == 3U,
               "BLE image DATA prefix changed");
_Static_assert(sizeof(ble_mesh_image_frame_t) == 2U,
               "BLE image control wire layout changed");
_Static_assert(sizeof(ble_mesh_image_nack_header_t) == 3U,
               "BLE image NACK prefix changed");
_Static_assert(sizeof(ble_mesh_image_reject_t) == 3U,
               "BLE image REJECT wire layout changed");
_Static_assert(BLE_MESH_IMAGE_MAX_CHUNKS == 83U,
               "BLE image chunk limit changed");
_Static_assert(sizeof(ble_mesh_image_data_header_t) +
                   BLE_MESH_IMAGE_DATA_BYTES == 377U,
               "BLE image DATA maximum changed");
#endif

#ifdef __cplusplus
}
#endif

#endif /* BLE_MESH_IMAGE_PROTOCOL_H */
