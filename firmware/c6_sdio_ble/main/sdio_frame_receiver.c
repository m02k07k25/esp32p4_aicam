/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdio_frame_receiver.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ble_mesh_image_source.h"
#include "esp_crc.h"
#include "esp_hosted_peer_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdio_frame_protocol.h"

#define FRAME_ASSEMBLY_TIMEOUT_US (5LL * 1000LL * 1000LL)
#define RECEIVER_EVENT_QUEUE_LEN  16U
#define STATUS_WORKER_STACK_SIZE  4096U
#define STATUS_WORKER_PRIORITY    5U
#define STATUS_WORKER_POLL_MS     100U

typedef enum {
    RECEIVER_IDLE = 0,
    RECEIVER_ASSEMBLING,
    RECEIVER_VERIFYING,
    RECEIVER_PENDING_BLE,
    RECEIVER_BLE_ACTIVE,
} receiver_state_t;

typedef enum {
    WORK_QUERY = 0,
    WORK_SEND_CONTROL,
    WORK_FRAME_COMPLETE,
    WORK_BLE_FRAME_DONE,
} worker_event_kind_t;

typedef struct {
    worker_event_kind_t kind;
    sdio_frame_control_state_t control_state;
    sdio_frame_control_reason_t reason;
    uint32_t frame_id;
    uint16_t ble_frame_id;
    uint32_t detail;
    esp_err_t result;
} worker_event_t;

typedef struct {
    receiver_state_t state;
    sdio_frame_chunk_header_t first_header;
    uint32_t received_bytes;
    uint16_t next_chunk;
    uint16_t ble_frame_id;
    int64_t last_activity_us;
} receiver_context_t;

typedef struct {
    bool valid;
    sdio_frame_control_state_t state;
    sdio_frame_control_reason_t reason;
    uint32_t frame_id;
    uint16_t ble_frame_id;
    uint32_t detail;
} terminal_control_cache_t;

static const char *TAG = "sdio_frame_rx";

/*
 * One statically allocated buffer is shared with BLE Mesh using a zero-copy
 * hand-off.  RECEIVER_BLE_ACTIVE prevents any SDIO writer from touching it
 * until the BLE frame_done callback has run.
 */
static uint8_t s_jpeg_buffer[SDIO_FRAME_MAX_JPEG_SIZE] __attribute__((aligned(4)));
static receiver_context_t s_receiver;
static terminal_control_cache_t s_last_terminal;
static portMUX_TYPE s_receiver_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_worker_queue;
static QueueHandle_t s_lifecycle_queue;
static TaskHandle_t s_worker_task;
static bool s_mesh_ready_event_pending;
static bool s_initialized;

static void cache_terminal_control(sdio_frame_control_state_t state,
                                   uint32_t frame_id,
                                   uint16_t ble_frame_id,
                                   sdio_frame_control_reason_t reason,
                                   uint32_t detail)
{
    if (frame_id == 0 ||
        (state != SDIO_FRAME_CONTROL_SERVER_ACKED &&
         state != SDIO_FRAME_CONTROL_FAILED)) {
        return;
    }

    taskENTER_CRITICAL(&s_receiver_lock);
    s_last_terminal.valid = true;
    s_last_terminal.state = state;
    s_last_terminal.reason = reason;
    s_last_terminal.frame_id = frame_id;
    s_last_terminal.ble_frame_id = ble_frame_id;
    s_last_terminal.detail = detail;
    taskEXIT_CRITICAL(&s_receiver_lock);
}

static bool enqueue_event(const worker_event_t *event)
{
    return s_worker_queue != NULL && xQueueSend(s_worker_queue, event, 0) == pdTRUE;
}

/*
 * Frame-complete and BLE-done cannot overlap: BLE cannot finish until the
 * worker has consumed frame-complete and submitted it.  A dedicated one-slot
 * overwrite mailbox therefore makes both lifecycle transitions lossless even
 * when QUERY traffic fills the ordinary status queue.
 */
static bool enqueue_lifecycle_event(const worker_event_t *event)
{
    return s_lifecycle_queue != NULL &&
           xQueueOverwrite(s_lifecycle_queue, event) == pdTRUE;
}

static void enqueue_control(sdio_frame_control_state_t state,
                            uint32_t frame_id,
                            uint16_t ble_frame_id,
                            sdio_frame_control_reason_t reason,
                            uint32_t detail)
{
    /* A full best-effort queue must not erase the result of an active frame.
     * Cache terminal controls before enqueue so QUERY can always recover them. */
    cache_terminal_control(state, frame_id, ble_frame_id, reason, detail);
    const worker_event_t event = {
        .kind = WORK_SEND_CONTROL,
        .control_state = state,
        .reason = reason,
        .frame_id = frame_id,
        .ble_frame_id = ble_frame_id,
        .detail = detail,
    };
    (void)enqueue_event(&event);
}

static void reset_receiver_locked(void)
{
    memset(&s_receiver.first_header, 0, sizeof(s_receiver.first_header));
    s_receiver.received_bytes = 0;
    s_receiver.next_chunk = 0;
    s_receiver.ble_frame_id = 0;
    s_receiver.last_activity_us = 0;
    s_receiver.state = RECEIVER_IDLE;
}

static bool metadata_matches_locked(const sdio_frame_chunk_header_t *header)
{
    const sdio_frame_chunk_header_t *first = &s_receiver.first_header;

    return header->frame_id == first->frame_id &&
           header->chunk_count == first->chunk_count &&
           header->jpeg_size == first->jpeg_size &&
           header->detected_at_ms == first->detected_at_ms &&
           header->jpeg_crc32 == first->jpeg_crc32;
}

static sdio_frame_control_reason_t validate_chunk_header(
    const sdio_frame_chunk_header_t *header,
    size_t data_len)
{
    if (header->magic != SDIO_FRAME_MAGIC ||
        header->version != SDIO_FRAME_VERSION ||
        header->header_size != sizeof(*header)) {
        return SDIO_FRAME_REASON_INVALID_HEADER;
    }

    if (header->jpeg_size == 0 || header->jpeg_size > SDIO_FRAME_MAX_JPEG_SIZE ||
        header->chunk_size == 0 || header->chunk_size > SDIO_FRAME_CHUNK_DATA_MAX ||
        data_len != sizeof(*header) + (size_t)header->chunk_size) {
        return SDIO_FRAME_REASON_SIZE;
    }

    if (header->chunk_count == 0 || header->chunk_index >= header->chunk_count ||
        header->chunk_offset > header->jpeg_size ||
        header->chunk_size > header->jpeg_size - header->chunk_offset) {
        return SDIO_FRAME_REASON_INVALID_HEADER;
    }

    const uint32_t known_flags = SDIO_FRAME_FLAG_FIRST_CHUNK |
                                 SDIO_FRAME_FLAG_LAST_CHUNK;
    const bool is_first = header->chunk_index == 0;
    const bool is_last = header->chunk_index == header->chunk_count - 1;
    if ((header->flags & ~known_flags) != 0 ||
        is_first != ((header->flags & SDIO_FRAME_FLAG_FIRST_CHUNK) != 0) ||
        is_last != ((header->flags & SDIO_FRAME_FLAG_LAST_CHUNK) != 0)) {
        return SDIO_FRAME_REASON_INVALID_HEADER;
    }

    return SDIO_FRAME_REASON_NONE;
}

static void reject_current_assembly(sdio_frame_control_reason_t reason,
                                    uint32_t detail,
                                    uint32_t fallback_frame_id)
{
    uint32_t frame_id = fallback_frame_id;

    taskENTER_CRITICAL(&s_receiver_lock);
    if (s_receiver.state == RECEIVER_ASSEMBLING ||
        s_receiver.state == RECEIVER_VERIFYING) {
        frame_id = s_receiver.first_header.frame_id;
        reset_receiver_locked();
    }
    taskEXIT_CRITICAL(&s_receiver_lock);

    enqueue_control(SDIO_FRAME_CONTROL_FAILED, frame_id, 0, reason, detail);
}

/* ESP-Hosted invokes this in its RPC RX task.  It never blocks or transmits. */
static void frame_chunk_callback(uint32_t msg_id, const uint8_t *data, size_t data_len)
{
    sdio_frame_chunk_header_t header;
    sdio_frame_chunk_header_t completed_header = {0};
    uint32_t active_frame_id = 0;
    uint32_t received_bytes = 0;
    uint16_t active_ble_frame_id = 0;
    bool frame_complete = false;

    if (msg_id != SDIO_FRAME_MSG_ID || data == NULL || data_len < sizeof(header)) {
        reject_current_assembly(SDIO_FRAME_REASON_INVALID_HEADER,
                                (uint32_t)data_len,
                                0);
        return;
    }

    /* The RPC payload is byte aligned, so never dereference it as a packed struct. */
    memcpy(&header, data, sizeof(header));
    const sdio_frame_control_reason_t validation =
        validate_chunk_header(&header, data_len);
    if (validation != SDIO_FRAME_REASON_NONE) {
        reject_current_assembly(validation, (uint32_t)data_len, header.frame_id);
        return;
    }

    const bool mesh_ready = ble_mesh_image_source_is_ready();

    taskENTER_CRITICAL(&s_receiver_lock);

    if (header.chunk_index == 0) {
        if (s_receiver.state != RECEIVER_IDLE) {
            active_frame_id = s_receiver.first_header.frame_id;
            active_ble_frame_id = s_receiver.ble_frame_id;
            received_bytes = s_receiver.received_bytes;

            /* A repeated first chunk for the active frame is an ordering error,
             * not a new-frame collision.  Drop the partial assembly so the P4
             * can restart it after observing FAILED. */
            if (s_receiver.state == RECEIVER_ASSEMBLING &&
                header.frame_id == active_frame_id) {
                const uint32_t detail =
                    ((uint32_t)s_receiver.next_chunk << 16) | header.chunk_index;
                reset_receiver_locked();
                taskEXIT_CRITICAL(&s_receiver_lock);
                enqueue_control(SDIO_FRAME_CONTROL_FAILED,
                                active_frame_id,
                                0,
                                SDIO_FRAME_REASON_ORDER,
                                detail);
                return;
            }

            taskEXIT_CRITICAL(&s_receiver_lock);
            enqueue_control(SDIO_FRAME_CONTROL_BUSY,
                            header.frame_id,
                            active_ble_frame_id,
                            SDIO_FRAME_REASON_BUSY,
                            active_frame_id);
            return;
        }
        if (!mesh_ready) {
            taskEXIT_CRITICAL(&s_receiver_lock);
            enqueue_control(SDIO_FRAME_CONTROL_FAILED,
                            header.frame_id,
                            0,
                            SDIO_FRAME_REASON_MESH_NOT_READY,
                            0);
            return;
        }

        s_receiver.state = RECEIVER_ASSEMBLING;
        s_receiver.first_header = header;
        s_receiver.received_bytes = 0;
        s_receiver.next_chunk = 0;
        s_receiver.ble_frame_id = 0;
        s_receiver.last_activity_us = esp_timer_get_time();
        if (s_last_terminal.valid &&
            s_last_terminal.frame_id == header.frame_id) {
            /* A new accepted first chunk supersedes a cached result that used
             * the same ID (for example after the P4 counter is reset). */
            memset(&s_last_terminal, 0, sizeof(s_last_terminal));
        }
    }

    if (s_receiver.state != RECEIVER_ASSEMBLING) {
        active_frame_id = s_receiver.first_header.frame_id;
        active_ble_frame_id = s_receiver.ble_frame_id;
        received_bytes = s_receiver.received_bytes;
        const receiver_state_t state = s_receiver.state;
        taskEXIT_CRITICAL(&s_receiver_lock);

        if (state == RECEIVER_PENDING_BLE || state == RECEIVER_BLE_ACTIVE ||
            state == RECEIVER_VERIFYING) {
            enqueue_control(SDIO_FRAME_CONTROL_BUSY,
                            header.frame_id,
                            active_ble_frame_id,
                            SDIO_FRAME_REASON_BUSY,
                            active_frame_id);
        } else {
            enqueue_control(SDIO_FRAME_CONTROL_FAILED,
                            header.frame_id,
                            0,
                            SDIO_FRAME_REASON_ORDER,
                            header.chunk_index);
        }
        return;
    }

    if (header.frame_id != s_receiver.first_header.frame_id) {
        active_frame_id = s_receiver.first_header.frame_id;
        received_bytes = s_receiver.received_bytes;
        taskEXIT_CRITICAL(&s_receiver_lock);
        enqueue_control(SDIO_FRAME_CONTROL_BUSY,
                        header.frame_id,
                        0,
                        SDIO_FRAME_REASON_BUSY,
                        active_frame_id);
        return;
    }

    if (!metadata_matches_locked(&header) ||
        header.chunk_index != s_receiver.next_chunk ||
        header.chunk_offset != s_receiver.received_bytes) {
        active_frame_id = s_receiver.first_header.frame_id;
        const uint32_t detail = ((uint32_t)s_receiver.next_chunk << 16) |
                                header.chunk_index;
        reset_receiver_locked();
        taskEXIT_CRITICAL(&s_receiver_lock);
        enqueue_control(SDIO_FRAME_CONTROL_FAILED,
                        active_frame_id,
                        0,
                        SDIO_FRAME_REASON_ORDER,
                        detail);
        return;
    }

    memcpy(s_jpeg_buffer + header.chunk_offset,
           data + header.header_size,
           header.chunk_size);
    s_receiver.received_bytes += header.chunk_size;
    s_receiver.next_chunk++;
    s_receiver.last_activity_us = esp_timer_get_time();

    if ((header.flags & SDIO_FRAME_FLAG_LAST_CHUNK) != 0) {
        if (s_receiver.received_bytes != s_receiver.first_header.jpeg_size ||
            s_receiver.next_chunk != s_receiver.first_header.chunk_count) {
            active_frame_id = s_receiver.first_header.frame_id;
            received_bytes = s_receiver.received_bytes;
            reset_receiver_locked();
            taskEXIT_CRITICAL(&s_receiver_lock);
            enqueue_control(SDIO_FRAME_CONTROL_FAILED,
                            active_frame_id,
                            0,
                            SDIO_FRAME_REASON_ORDER,
                            received_bytes);
            return;
        }
        s_receiver.state = RECEIVER_VERIFYING;
        completed_header = s_receiver.first_header;
        frame_complete = true;
    }

    taskEXIT_CRITICAL(&s_receiver_lock);

    if (!frame_complete) {
        return;
    }

    const uint32_t actual_crc =
        esp_crc32_le(0, s_jpeg_buffer, completed_header.jpeg_size);
    if (actual_crc != completed_header.jpeg_crc32) {
        reject_current_assembly(SDIO_FRAME_REASON_CRC,
                                actual_crc,
                                completed_header.frame_id);
        return;
    }

    const uint32_t jpeg_size = completed_header.jpeg_size;
    if (jpeg_size < 4 || s_jpeg_buffer[0] != 0xff || s_jpeg_buffer[1] != 0xd8 ||
        s_jpeg_buffer[jpeg_size - 2] != 0xff ||
        s_jpeg_buffer[jpeg_size - 1] != 0xd9) {
        reject_current_assembly(SDIO_FRAME_REASON_JPEG,
                                jpeg_size,
                                completed_header.frame_id);
        return;
    }

    taskENTER_CRITICAL(&s_receiver_lock);
    if (s_receiver.state != RECEIVER_VERIFYING ||
        s_receiver.first_header.frame_id != completed_header.frame_id) {
        taskEXIT_CRITICAL(&s_receiver_lock);
        return;
    }
    s_receiver.state = RECEIVER_PENDING_BLE;
    const uint32_t frame_id = completed_header.frame_id;
    taskEXIT_CRITICAL(&s_receiver_lock);

    const worker_event_t event = {
        .kind = WORK_FRAME_COMPLETE,
        .frame_id = frame_id,
        .detail = jpeg_size,
    };
    if (!enqueue_lifecycle_event(&event)) {
        taskENTER_CRITICAL(&s_receiver_lock);
        if (s_receiver.state == RECEIVER_PENDING_BLE &&
            s_receiver.first_header.frame_id == frame_id) {
            reset_receiver_locked();
        }
        taskEXIT_CRITICAL(&s_receiver_lock);
        enqueue_control(SDIO_FRAME_CONTROL_FAILED,
                        frame_id,
                        0,
                        SDIO_FRAME_REASON_QUEUE,
                        0);
    }
}

/* ESP-Hosted invokes this in its RPC RX task.  It only queues a reply request. */
static void control_callback(uint32_t msg_id, const uint8_t *data, size_t data_len)
{
    sdio_frame_control_t control = {0};

    if (msg_id != SDIO_FRAME_CONTROL_MSG_ID || data == NULL ||
        data_len != sizeof(control)) {
        enqueue_control(SDIO_FRAME_CONTROL_FAILED,
                        0,
                        0,
                        SDIO_FRAME_REASON_INVALID_HEADER,
                        (uint32_t)data_len);
        return;
    }

    memcpy(&control, data, sizeof(control));
    if (control.magic != SDIO_FRAME_CONTROL_MAGIC ||
        control.version != SDIO_FRAME_VERSION ||
        control.size != sizeof(control) ||
        control.state != SDIO_FRAME_CONTROL_QUERY) {
        enqueue_control(SDIO_FRAME_CONTROL_FAILED,
                        control.frame_id,
                        0,
                        SDIO_FRAME_REASON_INVALID_HEADER,
                        control.state);
        return;
    }

    const worker_event_t event = {
        .kind = WORK_QUERY,
        .frame_id = control.frame_id,
    };
    (void)enqueue_event(&event);
}

static void mesh_ready_changed_callback(bool ready, void *user_ctx)
{
    (void)ready;
    (void)user_ctx;

    /* Coalesce edges, but never lose the final readiness state. */
    taskENTER_CRITICAL(&s_receiver_lock);
    s_mesh_ready_event_pending = true;
    taskEXIT_CRITICAL(&s_receiver_lock);
}

static void mesh_frame_done_callback(uint32_t p4_frame_id,
                                     uint16_t ble_frame_id,
                                     esp_err_t status,
                                     void *user_ctx)
{
    (void)user_ctx;
    const worker_event_t event = {
        .kind = WORK_BLE_FRAME_DONE,
        .frame_id = p4_frame_id,
        .ble_frame_id = ble_frame_id,
        .result = status,
    };
    /* One outstanding BLE frame means the one-slot mailbox cannot overflow. */
    const bool queued = enqueue_lifecycle_event(&event);
    if (!queued) {
        /* Defensive release for assertion-disabled production builds. */
        taskENTER_CRITICAL(&s_receiver_lock);
        if (s_receiver.state == RECEIVER_BLE_ACTIVE &&
            s_receiver.first_header.frame_id == p4_frame_id &&
            s_receiver.ble_frame_id == ble_frame_id) {
            reset_receiver_locked();
            s_mesh_ready_event_pending = true;
        }
        taskEXIT_CRITICAL(&s_receiver_lock);
    }
    configASSERT(queued);
}

static esp_err_t send_control(sdio_frame_control_state_t state,
                              uint32_t frame_id,
                              uint16_t ble_frame_id,
                              sdio_frame_control_reason_t reason,
                              uint32_t detail)
{
    /* Cache before transport submission. If this very response is lost, a
     * later QUERY(frame_id) reproduces the terminal result exactly. */
    cache_terminal_control(state, frame_id, ble_frame_id, reason, detail);

    const sdio_frame_control_t control = {
        .magic = SDIO_FRAME_CONTROL_MAGIC,
        .version = SDIO_FRAME_VERSION,
        .size = sizeof(control),
        .state = (uint16_t)state,
        .frame_id = frame_id,
        .ble_frame_id = ble_frame_id,
        .reason = (uint16_t)reason,
        .detail = detail,
    };

    const esp_err_t ret = esp_hosted_send_custom_data(
        SDIO_FRAME_CONTROL_MSG_ID,
        (const uint8_t *)&control,
        sizeof(control));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "control send failed state=%u frame=%" PRIu32 ": %s",
                 (unsigned int)state,
                 frame_id,
                 esp_err_to_name(ret));
    }
    return ret;
}

static void reply_to_query(uint32_t query_frame_id)
{
    receiver_state_t state;
    uint32_t active_frame_id;
    uint16_t ble_frame_id;
    terminal_control_cache_t terminal;

    taskENTER_CRITICAL(&s_receiver_lock);
    state = s_receiver.state;
    active_frame_id = s_receiver.first_header.frame_id;
    ble_frame_id = s_receiver.ble_frame_id;
    terminal = s_last_terminal;
    taskEXIT_CRITICAL(&s_receiver_lock);

    if (query_frame_id != 0 && terminal.valid &&
        terminal.frame_id == query_frame_id) {
        (void)send_control(terminal.state,
                           terminal.frame_id,
                           terminal.ble_frame_id,
                           terminal.reason,
                           terminal.detail);
        return;
    }

    if (state != RECEIVER_IDLE || ble_mesh_image_source_is_busy()) {
        (void)send_control(SDIO_FRAME_CONTROL_BUSY,
                           query_frame_id != 0 ? query_frame_id : active_frame_id,
                           ble_frame_id,
                           SDIO_FRAME_REASON_BUSY,
                           active_frame_id);
    } else if (!ble_mesh_image_source_is_ready()) {
        (void)send_control(SDIO_FRAME_CONTROL_NOT_READY,
                           query_frame_id,
                           0,
                           SDIO_FRAME_REASON_MESH_NOT_READY,
                           0);
    } else {
        (void)send_control(SDIO_FRAME_CONTROL_READY,
                           query_frame_id,
                           0,
                           SDIO_FRAME_REASON_NONE,
                           SDIO_FRAME_MAX_JPEG_SIZE);
    }
}

static void push_ready_if_available(void)
{
    if (ble_mesh_image_source_is_ready() && !ble_mesh_image_source_is_busy()) {
        (void)send_control(SDIO_FRAME_CONTROL_READY,
                           0,
                           0,
                           SDIO_FRAME_REASON_NONE,
                           SDIO_FRAME_MAX_JPEG_SIZE);
    }
}

static void submit_completed_frame(uint32_t frame_id)
{
    size_t jpeg_size = 0;
    uint64_t detected_at_ms = 0;
    uint32_t jpeg_crc32 = 0;

    taskENTER_CRITICAL(&s_receiver_lock);
    if (s_receiver.state == RECEIVER_PENDING_BLE &&
        s_receiver.first_header.frame_id == frame_id) {
        jpeg_size = s_receiver.first_header.jpeg_size;
        detected_at_ms = s_receiver.first_header.detected_at_ms;
        jpeg_crc32 = s_receiver.first_header.jpeg_crc32;
    }
    taskEXIT_CRITICAL(&s_receiver_lock);

    if (jpeg_size == 0) {
        return;
    }

    uint16_t ble_frame_id = 0;
    const esp_err_t ret = ble_mesh_image_source_submit(s_jpeg_buffer,
                                                       jpeg_size,
                                                       frame_id,
                                                       detected_at_ms,
                                                       jpeg_crc32,
                                                       &ble_frame_id);

    taskENTER_CRITICAL(&s_receiver_lock);
    if (s_receiver.state == RECEIVER_PENDING_BLE &&
        s_receiver.first_header.frame_id == frame_id) {
        if (ret == ESP_OK) {
            s_receiver.state = RECEIVER_BLE_ACTIVE;
            s_receiver.ble_frame_id = ble_frame_id;
        } else {
            reset_receiver_locked();
        }
    }
    taskEXIT_CRITICAL(&s_receiver_lock);

    if (ret == ESP_OK) {
        (void)send_control(SDIO_FRAME_CONTROL_ACCEPTED,
                           frame_id,
                           ble_frame_id,
                           SDIO_FRAME_REASON_NONE,
                           (uint32_t)jpeg_size);
    } else {
        (void)send_control(SDIO_FRAME_CONTROL_FAILED,
                           frame_id,
                           0,
                           ret == ESP_ERR_INVALID_STATE
                               ? SDIO_FRAME_REASON_MESH_NOT_READY
                               : SDIO_FRAME_REASON_BLE_SEND,
                           (uint32_t)ret);
        push_ready_if_available();
    }
}

static void handle_ble_frame_done(const worker_event_t *event)
{
    bool matched = false;

    taskENTER_CRITICAL(&s_receiver_lock);
    if (s_receiver.state == RECEIVER_BLE_ACTIVE &&
        s_receiver.first_header.frame_id == event->frame_id &&
        s_receiver.ble_frame_id == event->ble_frame_id) {
        reset_receiver_locked();
        matched = true;
    }
    taskEXIT_CRITICAL(&s_receiver_lock);

    if (!matched) {
        ESP_LOGW(TAG,
                 "ignored stale BLE completion p4=%" PRIu32 " ble=%u",
                 event->frame_id,
                 (unsigned int)event->ble_frame_id);
        return;
    }

    if (event->result == ESP_OK) {
        (void)send_control(SDIO_FRAME_CONTROL_SERVER_ACKED,
                           event->frame_id,
                           event->ble_frame_id,
                           SDIO_FRAME_REASON_NONE,
                           0);
    } else {
        (void)send_control(SDIO_FRAME_CONTROL_FAILED,
                           event->frame_id,
                           event->ble_frame_id,
                           SDIO_FRAME_REASON_BLE_SEND,
                           (uint32_t)event->result);
    }
    push_ready_if_available();
}

static void check_assembly_timeout(void)
{
    uint32_t frame_id = 0;
    uint32_t received_bytes = 0;
    bool timed_out = false;
    const int64_t now = esp_timer_get_time();

    taskENTER_CRITICAL(&s_receiver_lock);
    if (s_receiver.state == RECEIVER_ASSEMBLING &&
        now - s_receiver.last_activity_us >= FRAME_ASSEMBLY_TIMEOUT_US) {
        frame_id = s_receiver.first_header.frame_id;
        received_bytes = s_receiver.received_bytes;
        reset_receiver_locked();
        timed_out = true;
    }
    taskEXIT_CRITICAL(&s_receiver_lock);

    if (timed_out) {
        (void)send_control(SDIO_FRAME_CONTROL_FAILED,
                           frame_id,
                           0,
                           SDIO_FRAME_REASON_TIMEOUT,
                           received_bytes);
    }
}

static void status_worker(void *arg)
{
    (void)arg;
    worker_event_t event;

    while (true) {
        /* Lifecycle events always outrank QUERY/control traffic. */
        if (xQueueReceive(s_lifecycle_queue, &event, 0) == pdTRUE) {
            if (event.kind == WORK_FRAME_COMPLETE) {
                submit_completed_frame(event.frame_id);
            } else if (event.kind == WORK_BLE_FRAME_DONE) {
                handle_ble_frame_done(&event);
            }
            check_assembly_timeout();
            continue;
        }

        bool ready_event_pending;
        taskENTER_CRITICAL(&s_receiver_lock);
        ready_event_pending = s_mesh_ready_event_pending;
        s_mesh_ready_event_pending = false;
        taskEXIT_CRITICAL(&s_receiver_lock);
        if (ready_event_pending) {
            reply_to_query(0);
            check_assembly_timeout();
            continue;
        }

        if (xQueueReceive(s_worker_queue,
                          &event,
                          pdMS_TO_TICKS(STATUS_WORKER_POLL_MS)) == pdTRUE) {
            switch (event.kind) {
            case WORK_QUERY:
                reply_to_query(event.frame_id);
                break;
            case WORK_SEND_CONTROL:
                (void)send_control(event.control_state,
                                   event.frame_id,
                                   event.ble_frame_id,
                                   event.reason,
                                   event.detail);
                break;
            default:
                break;
            }
        }

        check_assembly_timeout();
    }
}

esp_err_t sdio_frame_receiver_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_receiver_lock);
    reset_receiver_locked();
    memset(&s_last_terminal, 0, sizeof(s_last_terminal));
    taskEXIT_CRITICAL(&s_receiver_lock);

    s_worker_queue = xQueueCreate(RECEIVER_EVENT_QUEUE_LEN, sizeof(worker_event_t));
    if (s_worker_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_lifecycle_queue = xQueueCreate(1, sizeof(worker_event_t));
    if (s_lifecycle_queue == NULL) {
        vQueueDelete(s_worker_queue);
        s_worker_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(status_worker,
                    "sdio_status",
                    STATUS_WORKER_STACK_SIZE,
                    NULL,
                    STATUS_WORKER_PRIORITY,
                    &s_worker_task) != pdPASS) {
        vQueueDelete(s_worker_queue);
        s_worker_queue = NULL;
        vQueueDelete(s_lifecycle_queue);
        s_lifecycle_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    const ble_mesh_image_source_callbacks_t mesh_callbacks = {
        .ready_changed = mesh_ready_changed_callback,
        .frame_done = mesh_frame_done_callback,
    };
    esp_err_t ret = ble_mesh_image_source_register_callbacks(&mesh_callbacks, NULL);
    if (ret != ESP_OK) {
        goto fail;
    }

    ret = esp_hosted_register_custom_callback(SDIO_FRAME_MSG_ID,
                                               frame_chunk_callback);
    if (ret != ESP_OK) {
        goto fail;
    }

    ret = esp_hosted_register_custom_callback(SDIO_FRAME_CONTROL_MSG_ID,
                                               control_callback);
    if (ret != ESP_OK) {
        (void)esp_hosted_register_custom_callback(SDIO_FRAME_MSG_ID, NULL);
        goto fail;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "SDIO frame receiver ready: protocol v%u, max JPEG=%u bytes",
             SDIO_FRAME_VERSION,
             SDIO_FRAME_MAX_JPEG_SIZE);
    return ESP_OK;

fail:
    vTaskDelete(s_worker_task);
    s_worker_task = NULL;
    vQueueDelete(s_worker_queue);
    s_worker_queue = NULL;
    vQueueDelete(s_lifecycle_queue);
    s_lifecycle_queue = NULL;
    ESP_LOGE(TAG, "receiver init failed: %s", esp_err_to_name(ret));
    return ret;
}
