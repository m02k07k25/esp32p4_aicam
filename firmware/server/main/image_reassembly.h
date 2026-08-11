#ifndef IMAGE_REASSEMBLY_H
#define IMAGE_REASSEMBLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_mesh_image_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMAGE_REASSEMBLY_BITMAP_BYTES \
    ((BLE_MESH_IMAGE_MAX_CHUNKS + 7U) / 8U)
#define IMAGE_REASSEMBLY_REPLY_MAX       3U
#define IMAGE_REASSEMBLY_CACHE_ENTRIES   16U
#ifdef CONFIG_SERVER_FRAME_TIMEOUT_MS
#define IMAGE_REASSEMBLY_TIMEOUT_MS \
    ((uint64_t)CONFIG_SERVER_FRAME_TIMEOUT_MS)
#else
#define IMAGE_REASSEMBLY_TIMEOUT_MS      UINT64_C(30000)
#endif
#define IMAGE_REASSEMBLY_CACHE_MS        UINT64_C(60000)

typedef struct {
    uint8_t opcode;
    uint16_t destination;
    uint8_t payload[sizeof(ble_mesh_image_nack_header_t) +
                    BLE_MESH_IMAGE_NACK_BITMAP_MAX];
    size_t payload_len;
} image_reassembly_reply_t;

typedef struct {
    bool valid;
    uint16_t source_addr;
    uint16_t frame_id;
    uint64_t detected_at_ms;
    uint64_t rx_estimate_ms;
    const uint8_t *jpeg;
    size_t jpeg_len;
} image_reassembly_complete_t;

typedef struct {
    bool valid;
    uint16_t source_addr;
    uint16_t frame_id;
    uint64_t detected_at_ms;
    uint16_t image_len;
    uint32_t jpeg_crc32;
    uint64_t expires_at_ms;
} image_completion_cache_t;

typedef struct {
    bool active;
    uint16_t source_addr;
    uint16_t frame_id;
    uint64_t detected_at_ms;
    uint64_t rx_estimate_ms;
    uint16_t image_len;
    uint32_t jpeg_crc32;
    uint8_t total_chunks;
    uint64_t last_activity_ms;
    uint8_t received_bitmap[IMAGE_REASSEMBLY_BITMAP_BYTES];
    uint8_t jpeg[BLE_MESH_IMAGE_MAX_BYTES];
    image_completion_cache_t cache[IMAGE_REASSEMBLY_CACHE_ENTRIES];
    size_t next_cache;
} image_reassembly_t;

void image_reassembly_init(image_reassembly_t *state);

/*
 * Attach the server wall-clock sampled when a timestamp-less OPEN arrived.
 * The first nonzero value wins for the matching active session, so retries
 * and the later JPEG transfer cannot move the receive-time estimate.
 */
void image_reassembly_set_rx_estimate(image_reassembly_t *state,
                                      uint16_t source_addr,
                                      uint16_t frame_id,
                                      uint64_t rx_estimate_ms);

size_t image_reassembly_receive(image_reassembly_t *state,
                                uint16_t source_addr,
                                uint8_t opcode,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t now_ms,
                                image_reassembly_reply_t *replies,
                                size_t reply_capacity,
                                image_reassembly_complete_t *complete);

size_t image_reassembly_poll_timeout(image_reassembly_t *state,
                                     uint64_t now_ms,
                                     image_reassembly_reply_t *reply);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_REASSEMBLY_H */
