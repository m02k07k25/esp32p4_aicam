#include "image_reassembly.h"

#include <string.h>

#include "esp_crc.h"

static void clear_active(image_reassembly_t *state)
{
    state->active = false;
    state->source_addr = 0U;
    state->frame_id = 0U;
    state->detected_at_ms = 0U;
    state->rx_estimate_ms = 0U;
    state->image_len = 0U;
    state->jpeg_crc32 = 0U;
    state->total_chunks = 0U;
    state->last_activity_ms = 0U;
    memset(state->received_bitmap, 0, sizeof(state->received_bitmap));
}

void image_reassembly_init(image_reassembly_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

void image_reassembly_set_rx_estimate(image_reassembly_t *state,
                                      uint16_t source_addr,
                                      uint16_t frame_id,
                                      uint64_t rx_estimate_ms)
{
    if (state != NULL && state->active && rx_estimate_ms != 0U &&
        state->source_addr == source_addr && state->frame_id == frame_id &&
        state->detected_at_ms == 0U && state->rx_estimate_ms == 0U) {
        state->rx_estimate_ms = rx_estimate_ms;
    }
}

static void expire_cache(image_reassembly_t *state, uint64_t now_ms)
{
    for (size_t i = 0; i < IMAGE_REASSEMBLY_CACHE_ENTRIES; ++i) {
        if (state->cache[i].valid && now_ms >= state->cache[i].expires_at_ms) {
            state->cache[i].valid = false;
        }
    }
}

static image_completion_cache_t *find_cache(image_reassembly_t *state,
                                             uint16_t source_addr,
                                             uint16_t frame_id,
                                             uint64_t now_ms)
{
    expire_cache(state, now_ms);
    for (size_t i = 0; i < IMAGE_REASSEMBLY_CACHE_ENTRIES; ++i) {
        image_completion_cache_t *entry = &state->cache[i];
        if (entry->valid && entry->source_addr == source_addr &&
            entry->frame_id == frame_id) {
            return entry;
        }
    }
    return NULL;
}

static void store_cache(image_reassembly_t *state, uint64_t now_ms)
{
    image_completion_cache_t *entry = &state->cache[state->next_cache];
    *entry = (image_completion_cache_t) {
        .valid = true,
        .source_addr = state->source_addr,
        .frame_id = state->frame_id,
        .detected_at_ms = state->detected_at_ms,
        .image_len = state->image_len,
        .jpeg_crc32 = state->jpeg_crc32,
        .expires_at_ms = now_ms + IMAGE_REASSEMBLY_CACHE_MS,
    };
    state->next_cache = (state->next_cache + 1U) %
                        IMAGE_REASSEMBLY_CACHE_ENTRIES;
}

static size_t make_frame_reply(image_reassembly_reply_t *reply,
                               uint8_t opcode,
                               uint16_t destination,
                               uint16_t frame_id)
{
    reply->opcode = opcode;
    reply->destination = destination;
    reply->payload_len = sizeof(ble_mesh_image_frame_t);
    ble_mesh_image_put_le16(reply->payload, frame_id);
    return 1U;
}

static size_t make_reject(image_reassembly_reply_t *reply,
                          uint16_t destination,
                          uint16_t frame_id,
                          ble_mesh_image_reject_reason_t reason)
{
    reply->opcode = BLE_MESH_IMAGE_OP_REJECT;
    reply->destination = destination;
    reply->payload_len = sizeof(ble_mesh_image_reject_t);
    ble_mesh_image_put_le16(reply->payload, frame_id);
    reply->payload[2] = (uint8_t)reason;
    return 1U;
}

static bool chunk_received(const image_reassembly_t *state, uint8_t index)
{
    return (state->received_bitmap[index >> 3] &
            (uint8_t)(1U << (index & 7U))) != 0U;
}

static void mark_chunk_received(image_reassembly_t *state, uint8_t index)
{
    state->received_bitmap[index >> 3] |=
        (uint8_t)(1U << (index & 7U));
}

static bool all_chunks_received(const image_reassembly_t *state)
{
    for (uint8_t i = 0; i < state->total_chunks; ++i) {
        if (!chunk_received(state, i)) {
            return false;
        }
    }
    return true;
}

static bool is_sof_marker(uint8_t marker)
{
    return (marker >= 0xC0U && marker <= 0xCFU && marker != 0xC4U &&
            marker != 0xC8U && marker != 0xCCU);
}

static bool jpeg_is_224x224(const uint8_t *jpeg, size_t len)
{
    if (jpeg == NULL || len < 12U || jpeg[0] != 0xFFU ||
        jpeg[1] != 0xD8U || jpeg[len - 2U] != 0xFFU ||
        jpeg[len - 1U] != 0xD9U) {
        return false;
    }

    size_t pos = 2U;
    while (pos + 1U < len) {
        if (jpeg[pos] != 0xFFU) {
            return false;
        }
        while (pos < len && jpeg[pos] == 0xFFU) {
            ++pos;
        }
        if (pos >= len) {
            return false;
        }

        uint8_t marker = jpeg[pos++];
        if (marker == 0xD9U || marker == 0xDAU) {
            break;
        }
        if (marker == 0x01U || (marker >= 0xD0U && marker <= 0xD7U)) {
            continue;
        }
        if (pos + 2U > len) {
            return false;
        }
        uint16_t segment_len = ((uint16_t)jpeg[pos] << 8) | jpeg[pos + 1U];
        if (segment_len < 2U || pos + segment_len > len) {
            return false;
        }
        if (is_sof_marker(marker)) {
            if (segment_len < 8U) {
                return false;
            }
            uint16_t height = ((uint16_t)jpeg[pos + 3U] << 8) |
                              jpeg[pos + 4U];
            uint16_t width = ((uint16_t)jpeg[pos + 5U] << 8) |
                             jpeg[pos + 6U];
            return width == 224U && height == 224U;
        }
        pos += segment_len;
    }
    return false;
}

static size_t handle_open(image_reassembly_t *state,
                          uint16_t source_addr,
                          const uint8_t *payload,
                          size_t payload_len,
                          uint64_t now_ms,
                          image_reassembly_reply_t *reply)
{
    uint16_t frame_id = payload != NULL && payload_len >= 2U ?
                        ble_mesh_image_get_le16(payload) : 0U;
    if (payload == NULL || payload_len != sizeof(ble_mesh_image_open_t)) {
        return make_reject(reply, source_addr, frame_id,
                           BLE_MESH_IMAGE_REJECT_MALFORMED);
    }

    uint64_t detected_at_ms = ble_mesh_image_get_le64(payload + 2U);
    uint16_t image_len = ble_mesh_image_get_le16(payload + 10U);
    uint32_t jpeg_crc32 = ble_mesh_image_get_le32(payload + 12U);
    if (image_len == 0U || image_len > BLE_MESH_IMAGE_MAX_BYTES) {
        return make_reject(reply, source_addr, frame_id,
                           BLE_MESH_IMAGE_REJECT_SIZE);
    }

    image_completion_cache_t *cached =
        find_cache(state, source_addr, frame_id, now_ms);
    if (cached != NULL && cached->detected_at_ms == detected_at_ms &&
        cached->image_len == image_len &&
        cached->jpeg_crc32 == jpeg_crc32) {
        return make_frame_reply(reply, BLE_MESH_IMAGE_OP_COMPLETE,
                                source_addr, frame_id);
    }
    if (cached != NULL) {
        cached->valid = false;
    }

    if (state->active) {
        if (state->source_addr != source_addr || state->frame_id != frame_id) {
            return make_frame_reply(reply, BLE_MESH_IMAGE_OP_BUSY,
                                    source_addr, frame_id);
        }
        if (state->detected_at_ms == detected_at_ms &&
            state->image_len == image_len &&
            state->jpeg_crc32 == jpeg_crc32) {
            state->last_activity_ms = now_ms;
            return make_frame_reply(reply, BLE_MESH_IMAGE_OP_ACCEPT,
                                    source_addr, frame_id);
        }
        clear_active(state);
        return make_reject(reply, source_addr, frame_id,
                           BLE_MESH_IMAGE_REJECT_STATE);
    }

    state->active = true;
    state->source_addr = source_addr;
    state->frame_id = frame_id;
    state->detected_at_ms = detected_at_ms;
    state->rx_estimate_ms = 0U;
    state->image_len = image_len;
    state->jpeg_crc32 = jpeg_crc32;
    state->total_chunks = (uint8_t)(
        (image_len + BLE_MESH_IMAGE_DATA_BYTES - 1U) /
        BLE_MESH_IMAGE_DATA_BYTES);
    state->last_activity_ms = now_ms;
    memset(state->received_bitmap, 0, sizeof(state->received_bitmap));
    return make_frame_reply(reply, BLE_MESH_IMAGE_OP_ACCEPT,
                            source_addr, frame_id);
}

static size_t handle_data(image_reassembly_t *state,
                          uint16_t source_addr,
                          const uint8_t *payload,
                          size_t payload_len,
                          uint64_t now_ms,
                          image_reassembly_reply_t *reply)
{
    uint16_t frame_id = payload != NULL && payload_len >= 2U ?
                        ble_mesh_image_get_le16(payload) : 0U;
    if (payload == NULL ||
        payload_len <= sizeof(ble_mesh_image_data_header_t) ||
        payload_len > BLE_MESH_IMAGE_DATA_WIRE_MAX) {
        return make_reject(reply, source_addr, frame_id,
                           BLE_MESH_IMAGE_REJECT_MALFORMED);
    }
    if (!state->active || state->source_addr != source_addr ||
        state->frame_id != frame_id) {
        return make_frame_reply(reply, BLE_MESH_IMAGE_OP_RESTART,
                                source_addr, frame_id);
    }

    uint8_t chunk_index = payload[2];
    if (chunk_index >= state->total_chunks) {
        clear_active(state);
        return make_reject(reply, source_addr, frame_id,
                           BLE_MESH_IMAGE_REJECT_STATE);
    }

    size_t offset = (size_t)chunk_index * BLE_MESH_IMAGE_DATA_BYTES;
    size_t expected = state->image_len - offset;
    if (expected > BLE_MESH_IMAGE_DATA_BYTES) {
        expected = BLE_MESH_IMAGE_DATA_BYTES;
    }
    size_t data_len = payload_len - sizeof(ble_mesh_image_data_header_t);
    if (data_len != expected) {
        clear_active(state);
        return make_reject(reply, source_addr, frame_id,
                           BLE_MESH_IMAGE_REJECT_MALFORMED);
    }

    const uint8_t *data = payload + sizeof(ble_mesh_image_data_header_t);
    if (chunk_received(state, chunk_index)) {
        if (memcmp(state->jpeg + offset, data, data_len) != 0) {
            clear_active(state);
            return make_reject(reply, source_addr, frame_id,
                               BLE_MESH_IMAGE_REJECT_STATE);
        }
    } else {
        memcpy(state->jpeg + offset, data, data_len);
        mark_chunk_received(state, chunk_index);
    }
    state->last_activity_ms = now_ms;
    return 0U;
}

static size_t build_nacks(const image_reassembly_t *state,
                          image_reassembly_reply_t *replies,
                          size_t capacity)
{
    size_t count = 0U;
    for (uint8_t base = 0U; base < state->total_chunks;
         base = (uint8_t)(base + BLE_MESH_IMAGE_NACK_BITS_MAX)) {
        uint8_t bitmap[BLE_MESH_IMAGE_NACK_BITMAP_MAX] = {0};
        size_t bitmap_len = 0U;
        uint8_t stop = (uint8_t)(base + BLE_MESH_IMAGE_NACK_BITS_MAX);
        if (stop > state->total_chunks) {
            stop = state->total_chunks;
        }
        for (uint8_t index = base; index < stop; ++index) {
            if (!chunk_received(state, index)) {
                uint8_t relative = (uint8_t)(index - base);
                bitmap[relative >> 3] |= (uint8_t)(1U << (relative & 7U));
                size_t needed = (size_t)(relative >> 3) + 1U;
                if (needed > bitmap_len) {
                    bitmap_len = needed;
                }
            }
        }
        if (bitmap_len == 0U) {
            continue;
        }
        if (count >= capacity) {
            break;
        }
        image_reassembly_reply_t *reply = &replies[count++];
        reply->opcode = BLE_MESH_IMAGE_OP_NACK;
        reply->destination = state->source_addr;
        reply->payload_len = sizeof(ble_mesh_image_nack_header_t) + bitmap_len;
        ble_mesh_image_put_le16(reply->payload, state->frame_id);
        reply->payload[2] = base;
        memcpy(reply->payload + sizeof(ble_mesh_image_nack_header_t),
               bitmap, bitmap_len);
    }
    return count;
}

static size_t handle_end(image_reassembly_t *state,
                         uint16_t source_addr,
                         const uint8_t *payload,
                         size_t payload_len,
                         uint64_t now_ms,
                         image_reassembly_reply_t *replies,
                         size_t reply_capacity,
                         image_reassembly_complete_t *complete)
{
    uint16_t frame_id = payload != NULL && payload_len >= 2U ?
                        ble_mesh_image_get_le16(payload) : 0U;
    if (payload == NULL || payload_len != sizeof(ble_mesh_image_frame_t)) {
        return make_reject(&replies[0], source_addr, frame_id,
                           BLE_MESH_IMAGE_REJECT_MALFORMED);
    }
    if (!state->active || state->source_addr != source_addr ||
        state->frame_id != frame_id) {
        if (find_cache(state, source_addr, frame_id, now_ms) != NULL) {
            return make_frame_reply(&replies[0],
                                    BLE_MESH_IMAGE_OP_COMPLETE,
                                    source_addr, frame_id);
        }
        return make_frame_reply(&replies[0], BLE_MESH_IMAGE_OP_RESTART,
                                source_addr, frame_id);
    }

    state->last_activity_ms = now_ms;
    if (!all_chunks_received(state)) {
        return build_nacks(state, replies, reply_capacity);
    }

    uint32_t actual_crc = esp_crc32_le(0, state->jpeg, state->image_len);
    if (actual_crc != state->jpeg_crc32) {
        clear_active(state);
        return make_reject(&replies[0], source_addr, frame_id,
                           BLE_MESH_IMAGE_REJECT_CRC);
    }
    if (!jpeg_is_224x224(state->jpeg, state->image_len)) {
        clear_active(state);
        return make_reject(&replies[0], source_addr, frame_id,
                           BLE_MESH_IMAGE_REJECT_JPEG);
    }

    if (complete != NULL) {
        *complete = (image_reassembly_complete_t) {
            .valid = true,
            .source_addr = state->source_addr,
            .frame_id = state->frame_id,
            .detected_at_ms = state->detected_at_ms,
            .rx_estimate_ms = state->rx_estimate_ms,
            .jpeg = state->jpeg,
            .jpeg_len = state->image_len,
        };
    }
    store_cache(state, now_ms);
    clear_active(state);
    return make_frame_reply(&replies[0], BLE_MESH_IMAGE_OP_COMPLETE,
                            source_addr, frame_id);
}

size_t image_reassembly_receive(image_reassembly_t *state,
                                uint16_t source_addr,
                                uint8_t opcode,
                                const uint8_t *payload,
                                size_t payload_len,
                                uint64_t now_ms,
                                image_reassembly_reply_t *replies,
                                size_t reply_capacity,
                                image_reassembly_complete_t *complete)
{
    if (complete != NULL) {
        memset(complete, 0, sizeof(*complete));
    }
    if (state == NULL || replies == NULL || reply_capacity == 0U) {
        return 0U;
    }
    expire_cache(state, now_ms);

    switch (opcode) {
    case BLE_MESH_IMAGE_OP_OPEN:
        return handle_open(state, source_addr, payload, payload_len, now_ms,
                           &replies[0]);
    case BLE_MESH_IMAGE_OP_DATA:
        return handle_data(state, source_addr, payload, payload_len, now_ms,
                           &replies[0]);
    case BLE_MESH_IMAGE_OP_END:
        return handle_end(state, source_addr, payload, payload_len, now_ms,
                          replies, reply_capacity, complete);
    default:
        return 0U;
    }
}

size_t image_reassembly_poll_timeout(image_reassembly_t *state,
                                     uint64_t now_ms,
                                     image_reassembly_reply_t *reply)
{
    if (state == NULL || reply == NULL) {
        return 0U;
    }
    expire_cache(state, now_ms);
    if (!state->active || now_ms - state->last_activity_ms <
                              IMAGE_REASSEMBLY_TIMEOUT_MS) {
        return 0U;
    }

    uint16_t destination = state->source_addr;
    uint16_t frame_id = state->frame_id;
    clear_active(state);
    return make_reject(reply, destination, frame_id,
                       BLE_MESH_IMAGE_REJECT_TIMEOUT);
}
