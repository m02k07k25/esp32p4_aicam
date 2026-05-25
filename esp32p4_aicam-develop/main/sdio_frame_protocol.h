#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDIO_FRAME_MSG_ID             0x504A5047u
#define SDIO_FRAME_MAGIC              0x46544A50u
#define SDIO_FRAME_VERSION            1
#define SDIO_FRAME_CLASS_NAME_MAX     32
#define SDIO_FRAME_CHUNK_DATA_MAX     7600
#define SDIO_FRAME_FLAG_FIRST_CHUNK   (1U << 0)
#define SDIO_FRAME_FLAG_LAST_CHUNK    (1U << 1)

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
    int32_t class_id;
    float score;
    float inference_ms;
    float total_ms;
    char class_name[SDIO_FRAME_CLASS_NAME_MAX];
} sdio_frame_chunk_header_t;

#ifdef __cplusplus
}
#endif
