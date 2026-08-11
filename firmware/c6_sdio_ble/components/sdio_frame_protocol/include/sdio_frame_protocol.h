#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDIO_FRAME_MSG_ID               0x504A5047u
#define SDIO_FRAME_CONTROL_MSG_ID       0x5043544Cu
#define SDIO_FRAME_MAGIC                0x46544A50u
#define SDIO_FRAME_CONTROL_MAGIC        0x4354524Cu
#define SDIO_FRAME_VERSION              3U
#define SDIO_FRAME_CHUNK_DATA_MAX       7600U
#define SDIO_FRAME_MAX_JPEG_SIZE        30720U
#define SDIO_FRAME_FLAG_FIRST_CHUNK     (1U << 0)
#define SDIO_FRAME_FLAG_LAST_CHUNK      (1U << 1)

typedef enum {
    SDIO_FRAME_CONTROL_QUERY = 1,
    SDIO_FRAME_CONTROL_NOT_READY,
    SDIO_FRAME_CONTROL_READY,
    SDIO_FRAME_CONTROL_BUSY,
    SDIO_FRAME_CONTROL_ACCEPTED,
    SDIO_FRAME_CONTROL_SERVER_ACKED,
    SDIO_FRAME_CONTROL_FAILED,
} sdio_frame_control_state_t;

typedef enum {
    SDIO_FRAME_REASON_NONE = 0,
    SDIO_FRAME_REASON_MESH_NOT_READY,
    SDIO_FRAME_REASON_BUSY,
    SDIO_FRAME_REASON_INVALID_HEADER,
    SDIO_FRAME_REASON_SIZE,
    SDIO_FRAME_REASON_ORDER,
    SDIO_FRAME_REASON_TIMEOUT,
    SDIO_FRAME_REASON_CRC,
    SDIO_FRAME_REASON_JPEG,
    SDIO_FRAME_REASON_QUEUE,
    SDIO_FRAME_REASON_BLE_SEND,
    SDIO_FRAME_REASON_INTERNAL,
} sdio_frame_control_reason_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t frame_id;
    uint16_t chunk_index;
    uint16_t chunk_count;
    uint32_t flags;
    uint32_t chunk_offset;
    uint32_t chunk_size;
    uint32_t jpeg_size;
    uint64_t detected_at_ms;
    uint32_t jpeg_crc32;
} sdio_frame_chunk_header_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint16_t state;
    uint16_t reserved;
    uint32_t frame_id;
    uint16_t ble_frame_id;
    uint16_t reason;
    uint32_t detail;
} sdio_frame_control_t;

#ifdef __cplusplus
static_assert(offsetof(sdio_frame_chunk_header_t, detected_at_ms) == 32,
              "Unexpected SDIO frame detection timestamp offset");
static_assert(offsetof(sdio_frame_chunk_header_t, jpeg_crc32) == 40,
              "Unexpected SDIO frame CRC offset");
static_assert(sizeof(sdio_frame_chunk_header_t) == 44,
              "Unexpected SDIO frame chunk header size");
static_assert(sizeof(sdio_frame_control_t) == 24,
              "Unexpected SDIO frame control size");
static_assert(SDIO_FRAME_CONTROL_SERVER_ACKED == 6,
              "Unexpected SDIO server acknowledgement state value");
#else
_Static_assert(offsetof(sdio_frame_chunk_header_t, detected_at_ms) == 32,
               "Unexpected SDIO frame detection timestamp offset");
_Static_assert(offsetof(sdio_frame_chunk_header_t, jpeg_crc32) == 40,
               "Unexpected SDIO frame CRC offset");
_Static_assert(sizeof(sdio_frame_chunk_header_t) == 44,
               "Unexpected SDIO frame chunk header size");
_Static_assert(sizeof(sdio_frame_control_t) == 24,
               "Unexpected SDIO frame control size");
_Static_assert(SDIO_FRAME_CONTROL_SERVER_ACKED == 6,
               "Unexpected SDIO server acknowledgement state value");
#endif

#ifdef __cplusplus
}
#endif
