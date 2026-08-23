#include "sdio_frame_tx.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "sdio_time_clock.h"

#if CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
#include "esp_event.h"
#include "esp_hosted.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sdio_frame_tx";

#define SDIO_FRAME_TX_QUEUE_LENGTH       1
#define SDIO_FRAME_TX_TASK_STACK_SIZE    6144
#define SDIO_FRAME_TX_TASK_PRIORITY      4
#define SDIO_FRAME_TX_POLL_MS            250
#define SDIO_FRAME_TX_SETUP_RETRY_MS     1000
#define SDIO_FRAME_TX_QUERY_INTERVAL_MS  2000
#define SDIO_FRAME_TX_RECONNECT_STEPS    5

typedef struct {
    uint32_t frame_id;
    uint32_t jpeg_crc32;
    uint64_t detected_at_ms;
    size_t jpeg_len;
    uint8_t jpeg[];
} sdio_frame_tx_job_t;

static QueueHandle_t s_tx_queue;
static SemaphoreHandle_t s_state_lock;
static bool s_hosted_initialized;
static bool s_hosted_event_registered;
static bool s_control_callback_registered;
static bool s_time_callback_registered;
static bool s_transport_up;
static bool s_query_pending;
static bool s_time_query_pending;
static bool s_time_query_outstanding;
static uint16_t s_remote_state = SDIO_FRAME_CONTROL_NOT_READY;
static uint32_t s_active_frame_id;
static bool s_active_frame_tx_complete;
static uint32_t s_next_frame_id;
static uint32_t s_next_time_request_id;
static uint32_t s_time_request_id;
static uint64_t s_time_client_tx_us;
static int64_t s_next_time_query_us;
static sdio_time_clock_t s_time_clock;
static TickType_t s_next_reconnect_tick;
static uint8_t s_reconnect_step;
static esp_event_handler_instance_t s_hosted_event_instance;
static const uint32_t s_reconnect_backoff_ms[SDIO_FRAME_TX_RECONNECT_STEPS] = {
    1000, 2000, 4000, 8000, 10000,
};

static void invalidate_time_locked(void)
{
    sdio_time_clock_invalidate(&s_time_clock);
    s_time_query_outstanding = false;
    s_time_request_id = 0;
    s_time_client_tx_us = 0;
}

static const char *control_state_name(uint16_t state)
{
    switch (state) {
    case SDIO_FRAME_CONTROL_QUERY:
        return "QUERY";
    case SDIO_FRAME_CONTROL_NOT_READY:
        return "NOT_READY";
    case SDIO_FRAME_CONTROL_READY:
        return "READY";
    case SDIO_FRAME_CONTROL_BUSY:
        return "BUSY";
    case SDIO_FRAME_CONTROL_ACCEPTED:
        return "ACCEPTED";
    case SDIO_FRAME_CONTROL_SERVER_ACKED:
        return "SERVER_ACKED";
    case SDIO_FRAME_CONTROL_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

static void state_mark_transport_down(void)
{
    if (s_state_lock == NULL || xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    const bool was_up = s_transport_up;
    s_transport_up = false;
    s_remote_state = SDIO_FRAME_CONTROL_NOT_READY;
    s_active_frame_id = 0;
    s_active_frame_tx_complete = false;
    s_query_pending = false;
    s_time_query_pending = false;
    invalidate_time_locked();
    if (was_up || s_next_reconnect_tick == 0) {
        s_next_reconnect_tick =
            xTaskGetTickCount() + pdMS_TO_TICKS(s_reconnect_backoff_ms[0]);
        s_reconnect_step = 1;
    }
    xSemaphoreGive(s_state_lock);
}

static void state_mark_transport_up(void)
{
    if (s_state_lock == NULL || xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    s_transport_up = true;
    s_remote_state = SDIO_FRAME_CONTROL_NOT_READY;
    s_active_frame_id = 0;
    s_active_frame_tx_complete = false;
    s_query_pending = true;
    s_time_query_pending = false;
    invalidate_time_locked();
    s_next_time_query_us = 0;
    s_next_reconnect_tick = 0;
    s_reconnect_step = 0;
    xSemaphoreGive(s_state_lock);
}

static bool state_claim_reconnect_attempt(TickType_t now, uint32_t *next_delay_ms)
{
    bool claimed = false;
    if (s_state_lock == NULL || xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    if (!s_transport_up &&
        (s_next_reconnect_tick == 0 || (int32_t)(now - s_next_reconnect_tick) >= 0)) {
        const uint8_t step = s_reconnect_step < SDIO_FRAME_TX_RECONNECT_STEPS
                                 ? s_reconnect_step
                                 : SDIO_FRAME_TX_RECONNECT_STEPS - 1;
        *next_delay_ms = s_reconnect_backoff_ms[step];
        s_next_reconnect_tick = now + pdMS_TO_TICKS(*next_delay_ms);
        if (s_reconnect_step < SDIO_FRAME_TX_RECONNECT_STEPS - 1) {
            ++s_reconnect_step;
        }
        claimed = true;
    }

    xSemaphoreGive(s_state_lock);
    return claimed;
}

static void hosted_event_handler(void *arg,
                                 esp_event_base_t event_base,
                                 int32_t event_id,
                                 void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base != ESP_HOSTED_EVENT) {
        return;
    }

    switch (event_id) {
    case ESP_HOSTED_EVENT_TRANSPORT_UP:
        state_mark_transport_up();
        ESP_LOGI(TAG, "ESP-Hosted transport UP; scheduling C6 readiness QUERY");
        break;
    case ESP_HOSTED_EVENT_TRANSPORT_DOWN:
        state_mark_transport_down();
        ESP_LOGW(TAG, "ESP-Hosted transport DOWN; reconnect starts in 1 second");
        break;
    case ESP_HOSTED_EVENT_TRANSPORT_FAILURE:
        state_mark_transport_down();
        ESP_LOGW(TAG, "ESP-Hosted transport FAILURE; reconnect starts in 1 second");
        break;
    case ESP_HOSTED_EVENT_CP_INIT:
        if (s_state_lock != NULL && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
            s_remote_state = SDIO_FRAME_CONTROL_NOT_READY;
            s_active_frame_id = 0;
            s_active_frame_tx_complete = false;
            s_query_pending = s_transport_up;
            s_time_query_pending = false;
            invalidate_time_locked();
            s_next_time_query_us = 0;
            xSemaphoreGive(s_state_lock);
        }
        ESP_LOGI(TAG, "C6 initialization event; readiness will be queried again");
        break;
    default:
        break;
    }
}

static void control_message_callback(uint32_t msg_id, const uint8_t *data, size_t data_len)
{
    if (msg_id != SDIO_FRAME_CONTROL_MSG_ID || data == NULL ||
        data_len != sizeof(sdio_frame_control_t)) {
        ESP_LOGW(TAG, "discarding malformed control message (%u B)", (unsigned int)data_len);
        return;
    }

    sdio_frame_control_t control;
    memcpy(&control, data, sizeof(control));
    if (control.magic != SDIO_FRAME_CONTROL_MAGIC ||
        control.version != SDIO_FRAME_VERSION ||
        control.size != sizeof(control) ||
        control.state < SDIO_FRAME_CONTROL_NOT_READY ||
        control.state > SDIO_FRAME_CONTROL_FAILED) {
        ESP_LOGW(TAG,
                 "discarding invalid control magic=%08" PRIx32 " version=%u size=%u state=%u",
                 control.magic,
                 control.version,
                 control.size,
                 control.state);
        return;
    }

    bool accepted = false;
    uint16_t previous_state = SDIO_FRAME_CONTROL_NOT_READY;
    if (s_state_lock != NULL && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        previous_state = s_remote_state;
        switch (control.state) {
        case SDIO_FRAME_CONTROL_READY:
            /*
             * READY never proves the active frame reached the Mesh server.
             * While a frame is in flight, C6 replays its cached terminal
             * SERVER_ACKED or FAILED response to a matching QUERY instead.
             */
            if (s_active_frame_id == 0) {
                s_remote_state = control.state;
                if (!s_time_clock.valid || s_next_time_query_us == 0) {
                    s_time_query_pending = true;
                }
                accepted = true;
            }
            break;
        case SDIO_FRAME_CONTROL_NOT_READY:
            if (s_active_frame_id == 0) {
                s_remote_state = control.state;
                invalidate_time_locked();
                s_time_query_pending = false;
                accepted = true;
            } else if (control.frame_id == s_active_frame_id) {
                /*
                 * C6 became unavailable after its READY grant. A response
                 * echoing our frame ID cancels this reservation; the worker
                 * notices the cleared ID and readiness polling resumes.
                 */
                s_remote_state = control.state;
                s_active_frame_id = 0;
                s_active_frame_tx_complete = false;
                s_query_pending = true;
                invalidate_time_locked();
                s_time_query_pending = false;
                accepted = true;
            }
            break;
        case SDIO_FRAME_CONTROL_BUSY:
            if (s_active_frame_id == 0) {
                s_remote_state = control.state;
                accepted = true;
            } else if (s_active_frame_tx_complete &&
                       control.frame_id == s_active_frame_id &&
                       control.detail == s_active_frame_id) {
                /*
                 * The same frame still owns C6's SDIO/BLE buffer. Keep its
                 * reservation and continue QUERY polling until C6 replays a
                 * cached SERVER_ACKED or FAILED terminal result.
                 */
                s_remote_state = control.state;
                accepted = true;
            } else if (control.frame_id == s_active_frame_id) {
                /* Another C6 assembly owns the buffer; abandon this job. */
                s_remote_state = control.state;
                s_active_frame_id = 0;
                s_active_frame_tx_complete = false;
                s_query_pending = true;
                accepted = true;
            }
            break;
        case SDIO_FRAME_CONTROL_ACCEPTED:
            if (s_active_frame_id != 0 && control.frame_id == s_active_frame_id) {
                s_remote_state = control.state;
                accepted = true;
            }
            break;
        case SDIO_FRAME_CONTROL_SERVER_ACKED:
        case SDIO_FRAME_CONTROL_FAILED:
            if (s_active_frame_id != 0 && control.frame_id == s_active_frame_id) {
                s_remote_state = control.state;
                s_active_frame_id = 0;
                s_active_frame_tx_complete = false;
                s_query_pending = true;
                accepted = true;
            }
            break;
        default:
            break;
        }

        xSemaphoreGive(s_state_lock);
    }

    if (accepted) {
        ESP_LOGI(TAG,
                 "C6 control %s -> %s frame=%" PRIu32 " ble_frame=%u reason=%u detail=%" PRIu32,
                 control_state_name(previous_state),
                 control_state_name(control.state),
                 control.frame_id,
                 control.ble_frame_id,
                 control.reason,
                 control.detail);
    } else {
        ESP_LOGD(TAG,
                 "ignored stale C6 control state=%s frame=%" PRIu32,
                 control_state_name(control.state),
                 control.frame_id);
    }
}

static void time_message_callback(uint32_t msg_id,
                                  const uint8_t *data,
                                  size_t data_len)
{
    const int64_t client_rx_us = esp_timer_get_time();
    if (msg_id != SDIO_TIME_MSG_ID || data == NULL ||
        data_len != sizeof(sdio_time_message_t)) {
        ESP_LOGW(TAG, "discarding malformed TIME message (%u B)",
                 (unsigned int)data_len);
        return;
    }

    sdio_time_message_t sample;
    memcpy(&sample, data, sizeof(sample));
    if (sample.magic != SDIO_TIME_MAGIC ||
        sample.version != SDIO_FRAME_VERSION ||
        sample.size != sizeof(sample) ||
        sample.kind != SDIO_TIME_KIND_SAMPLE ||
        sample.status < SDIO_TIME_STATUS_OK ||
        sample.status > SDIO_TIME_STATUS_FAILED) {
        ESP_LOGW(TAG,
                 "discarding invalid TIME magic=%08" PRIx32
                 " version=%u size=%u kind=%u status=%u",
                 sample.magic, sample.version, sample.size,
                 sample.kind, sample.status);
        return;
    }

    sdio_time_apply_result_t result = SDIO_TIME_APPLY_STALE;
    uint64_t measured_delay_us = 0;
    bool matched = false;
    if (s_state_lock != NULL &&
        xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        if (s_time_query_outstanding) {
            result = sdio_time_clock_apply_sample(
                &s_time_clock, &sample, s_time_request_id,
                s_time_client_tx_us, client_rx_us);
            matched = result != SDIO_TIME_APPLY_STALE;
        }

        if (matched) {
            s_time_query_outstanding = false;
            s_time_request_id = 0;
            s_time_client_tx_us = 0;
            s_time_query_pending = false;

            if (result == SDIO_TIME_APPLY_ACCEPTED) {
                measured_delay_us = s_time_clock.measured_delay_us;
                s_next_time_query_us =
                    client_rx_us + SDIO_TIME_QUERY_INTERVAL_US;
            } else if (result == SDIO_TIME_APPLY_REMOTE_STATUS &&
                       sample.status == SDIO_TIME_STATUS_BUSY) {
                /* BUSY is transient and does not discredit a still-fresh
                 * previously accepted mapping. */
                s_next_time_query_us =
                    client_rx_us + SDIO_TIME_BUSY_RETRY_US;
            } else {
                sdio_time_clock_invalidate(&s_time_clock);
                s_next_time_query_us =
                    client_rx_us + SDIO_TIME_ERROR_RETRY_US;
            }
        }
        xSemaphoreGive(s_state_lock);
    }

    if (!matched) {
        ESP_LOGD(TAG,
                 "ignored stale TIME sample request=%" PRIu32
                 " client_tx=%" PRIu64,
                 sample.request_id, sample.client_tx_monotonic_us);
    } else if (result == SDIO_TIME_APPLY_ACCEPTED) {
        ESP_LOGI(TAG,
                 "server clock synchronized request=%" PRIu32
                 " epoch_ms=%" PRIu64 " measured_delay=%" PRIu64 " us",
                 sample.request_id,
                 sdio_frame_tx_capture_time_ms(),
                 measured_delay_us);
    } else if (result == SDIO_TIME_APPLY_REMOTE_STATUS) {
        ESP_LOGW(TAG, "server clock unavailable request=%" PRIu32
                      " status=%u",
                 sample.request_id, sample.status);
    } else {
        ESP_LOGW(TAG, "rejected inconsistent TIME sample request=%" PRIu32,
                 sample.request_id);
    }
}

static esp_err_t ensure_hosted_runtime(void)
{
    esp_err_t ret;

    if (!s_hosted_event_registered) {
        ret = esp_event_handler_instance_register(ESP_HOSTED_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  hosted_event_handler,
                                                  NULL,
                                                  &s_hosted_event_instance);
        if (ret != ESP_OK) {
            return ret;
        }
        s_hosted_event_registered = true;
    }

    if (!s_hosted_initialized) {
        ret = esp_hosted_init();
        if (ret != ESP_OK) {
            return ret;
        }
        s_hosted_initialized = true;
        ESP_LOGI(TAG, "ESP-Hosted initialized explicitly");
    }

    if (!s_control_callback_registered) {
        ret = esp_hosted_register_custom_callback(SDIO_FRAME_CONTROL_MSG_ID,
                                                  control_message_callback);
        if (ret != ESP_OK) {
            return ret;
        }
        s_control_callback_registered = true;
        ESP_LOGI(TAG, "registered C6 control callback");
    }

    if (!s_time_callback_registered) {
        ret = esp_hosted_register_custom_callback(SDIO_TIME_MSG_ID,
                                                  time_message_callback);
        if (ret != ESP_OK) {
            return ret;
        }
        s_time_callback_registered = true;
        ESP_LOGI(TAG, "registered C6 server-clock callback");
    }

    return ESP_OK;
}

static bool hosted_runtime_ready(void)
{
    return s_hosted_event_registered && s_hosted_initialized &&
           s_control_callback_registered && s_time_callback_registered;
}

static esp_err_t send_control_query(void)
{
    sdio_frame_control_t query = {
        .magic = SDIO_FRAME_CONTROL_MAGIC,
        .version = SDIO_FRAME_VERSION,
        .size = sizeof(query),
        .state = SDIO_FRAME_CONTROL_QUERY,
        .reason = SDIO_FRAME_REASON_NONE,
    };

    if (s_state_lock != NULL && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        query.frame_id = s_active_frame_id;
        xSemaphoreGive(s_state_lock);
    }

    esp_err_t ret = esp_hosted_send_custom_data(
        SDIO_FRAME_CONTROL_MSG_ID, (const uint8_t *)&query, sizeof(query));
    if (ret != ESP_OK) {
        state_mark_transport_down();
    }
    return ret;
}

static bool prepare_time_query(int64_t now_us, sdio_time_message_t *query)
{
    bool prepared = false;
    if (query == NULL || s_state_lock == NULL ||
        xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    if (s_time_query_outstanding &&
        (now_us < 0 || s_time_client_tx_us > (uint64_t)INT64_MAX ||
         now_us - (int64_t)s_time_client_tx_us >=
             SDIO_TIME_QUERY_TIMEOUT_US)) {
        ESP_LOGW(TAG, "server-clock query timed out request=%" PRIu32,
                 s_time_request_id);
        s_time_query_outstanding = false;
        s_time_request_id = 0;
        s_time_client_tx_us = 0;
        s_next_time_query_us = now_us;
    }

    if (now_us >= 0 && s_transport_up && s_time_callback_registered &&
        s_remote_state == SDIO_FRAME_CONTROL_READY &&
        s_active_frame_id == 0 && !s_time_query_outstanding &&
        (s_time_query_pending || s_next_time_query_us == 0 ||
         now_us >= s_next_time_query_us)) {
        do {
            ++s_next_time_request_id;
        } while (s_next_time_request_id == 0);

        *query = (sdio_time_message_t) {
            .magic = SDIO_TIME_MAGIC,
            .version = SDIO_FRAME_VERSION,
            .size = sizeof(*query),
            .kind = SDIO_TIME_KIND_QUERY,
            .status = SDIO_TIME_STATUS_NONE,
            .request_id = s_next_time_request_id,
            .client_tx_monotonic_us = (uint64_t)now_us,
        };
        s_time_query_outstanding = true;
        s_time_query_pending = false;
        s_time_request_id = query->request_id;
        s_time_client_tx_us = query->client_tx_monotonic_us;
        /* A response normally chooses the next deadline. This also prevents
         * a tight retry loop if an unexpected callback clears the request. */
        s_next_time_query_us = now_us + SDIO_TIME_QUERY_TIMEOUT_US;
        prepared = true;
    }

    xSemaphoreGive(s_state_lock);
    return prepared;
}

static esp_err_t send_time_query(const sdio_time_message_t *query)
{
    if (query == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = esp_hosted_send_custom_data(
        SDIO_TIME_MSG_ID, (const uint8_t *)query, sizeof(*query));
    if (ret != ESP_OK) {
        state_mark_transport_down();
    }
    return ret;
}

static bool state_job_is_active(uint32_t frame_id);

static esp_err_t send_jpeg(const sdio_frame_tx_job_t *job, bool *transport_fault)
{
    *transport_fault = false;
    if (job == NULL || job->jpeg_len == 0 || job->jpeg_len > SDIO_FRAME_MAX_JPEG_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t header_size = sizeof(sdio_frame_chunk_header_t);
    const size_t tx_buf_size = header_size + SDIO_FRAME_CHUNK_DATA_MAX;
    uint8_t *tx_buf = (uint8_t *)malloc(tx_buf_size);
    if (tx_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const uint16_t chunk_count =
        (uint16_t)((job->jpeg_len + SDIO_FRAME_CHUNK_DATA_MAX - 1) /
                   SDIO_FRAME_CHUNK_DATA_MAX);
    esp_err_t ret = ESP_OK;

    for (uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        if (!state_job_is_active(job->frame_id)) {
            ret = ESP_ERR_INVALID_STATE;
            ESP_LOGW(TAG,
                     "C6 cancelled frame=%" PRIu32 " before chunk=%u/%u",
                     job->frame_id,
                     (unsigned int)chunk_index + 1,
                     (unsigned int)chunk_count);
            break;
        }

        const size_t offset = (size_t)chunk_index * SDIO_FRAME_CHUNK_DATA_MAX;
        const size_t remaining = job->jpeg_len - offset;
        const size_t chunk_size = remaining > SDIO_FRAME_CHUNK_DATA_MAX
                                      ? SDIO_FRAME_CHUNK_DATA_MAX
                                      : remaining;

        sdio_frame_chunk_header_t header = {
            .magic = SDIO_FRAME_MAGIC,
            .version = SDIO_FRAME_VERSION,
            .header_size = (uint16_t)header_size,
            .frame_id = job->frame_id,
            .chunk_index = chunk_index,
            .chunk_count = chunk_count,
            .flags = 0,
            .chunk_offset = (uint32_t)offset,
            .chunk_size = (uint32_t)chunk_size,
            .jpeg_size = (uint32_t)job->jpeg_len,
            .detected_at_ms = job->detected_at_ms,
            .jpeg_crc32 = job->jpeg_crc32,
        };
        if (chunk_index == 0) {
            header.flags |= SDIO_FRAME_FLAG_FIRST_CHUNK;
        }
        if (chunk_index == chunk_count - 1) {
            header.flags |= SDIO_FRAME_FLAG_LAST_CHUNK;
        }
        memcpy(tx_buf, &header, header_size);
        memcpy(tx_buf + header_size, job->jpeg + offset, chunk_size);

        ret = esp_hosted_send_custom_data(
            SDIO_FRAME_MSG_ID, tx_buf, header_size + chunk_size);
        if (ret != ESP_OK) {
            *transport_fault = true;
            ESP_LOGW(TAG,
                     "SDIO send failed frame=%" PRIu32 " chunk=%u/%u: %s",
                     job->frame_id,
                     (unsigned int)chunk_index + 1,
                     (unsigned int)chunk_count,
                     esp_err_to_name(ret));
            break;
        }
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "sent frame=%" PRIu32 " jpeg=%u B crc=%08" PRIx32
                 " chunks=%u detected_at_ms=%" PRIu64,
                 job->frame_id,
                 (unsigned int)job->jpeg_len,
                 job->jpeg_crc32,
                 (unsigned int)chunk_count,
                 job->detected_at_ms);
    }

    free(tx_buf);
    return ret;
}

static void free_job(sdio_frame_tx_job_t *job)
{
    free(job);
}

static bool state_should_query(TickType_t now, TickType_t last_query)
{
    bool should_query = false;
    if (s_state_lock == NULL || xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    if (s_transport_up && s_control_callback_registered &&
        (s_query_pending ||
         (s_remote_state != SDIO_FRAME_CONTROL_READY &&
          (last_query == 0 ||
           now - last_query >= pdMS_TO_TICKS(SDIO_FRAME_TX_QUERY_INTERVAL_MS))))) {
        s_query_pending = false;
        should_query = true;
    }

    xSemaphoreGive(s_state_lock);
    return should_query;
}

static bool state_job_is_active(uint32_t frame_id)
{
    bool active = false;
    if (s_state_lock != NULL && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        active = s_transport_up && s_active_frame_id == frame_id &&
                 s_remote_state == SDIO_FRAME_CONTROL_BUSY;
        xSemaphoreGive(s_state_lock);
    }
    return active;
}

static void state_mark_frame_tx_complete(uint32_t frame_id)
{
    if (s_state_lock != NULL && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        if (s_transport_up && s_active_frame_id == frame_id &&
            (s_remote_state == SDIO_FRAME_CONTROL_BUSY ||
             s_remote_state == SDIO_FRAME_CONTROL_ACCEPTED)) {
            s_active_frame_tx_complete = true;
            s_query_pending = true;
        }
        xSemaphoreGive(s_state_lock);
    }
}

static void state_release_frame_for_query(uint32_t frame_id)
{
    if (s_state_lock != NULL && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        if (s_active_frame_id == frame_id) {
            s_active_frame_id = 0;
            s_active_frame_tx_complete = false;
            s_remote_state = SDIO_FRAME_CONTROL_NOT_READY;
            s_query_pending = s_transport_up;
        }
        xSemaphoreGive(s_state_lock);
    }
}

static void sdio_frame_tx_worker(void *arg)
{
    (void)arg;
    TickType_t last_setup_attempt = 0;
    TickType_t last_query = 0;

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        if (!hosted_runtime_ready() &&
            (last_setup_attempt == 0 ||
             now - last_setup_attempt >= pdMS_TO_TICKS(SDIO_FRAME_TX_SETUP_RETRY_MS))) {
            last_setup_attempt = now;
            esp_err_t ret = ensure_hosted_runtime();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "ESP-Hosted setup failed: %s; retry in 1 second",
                         esp_err_to_name(ret));
            }
        }

        uint32_t next_delay_ms = 0;
        if (state_claim_reconnect_attempt(now, &next_delay_ms)) {
            if (!hosted_runtime_ready()) {
                ESP_LOGW(TAG,
                         "ESP-Hosted runtime is not ready; connect retry in %" PRIu32 " ms",
                         next_delay_ms);
            } else {
                esp_err_t ret = esp_hosted_connect_to_slave();
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG,
                             "ESP-Hosted reconnect request failed: %s; retry in %" PRIu32 " ms",
                             esp_err_to_name(ret),
                             next_delay_ms);
                } else {
                    ESP_LOGI(TAG,
                             "ESP-Hosted connect requested; waiting for TRANSPORT_UP"
                             " (next retry in %" PRIu32 " ms)",
                             next_delay_ms);
                }
            }
        }

        if (state_should_query(now, last_query)) {
            last_query = now;
            esp_err_t ret = send_control_query();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "C6 readiness QUERY failed: %s", esp_err_to_name(ret));
            }
        }

        sdio_time_message_t time_query;
        if (prepare_time_query(esp_timer_get_time(), &time_query)) {
            esp_err_t ret = send_time_query(&time_query);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "server-clock QUERY failed request=%" PRIu32 ": %s",
                         time_query.request_id, esp_err_to_name(ret));
            } else {
                ESP_LOGD(TAG, "server-clock QUERY sent request=%" PRIu32,
                         time_query.request_id);
            }
        }

        sdio_frame_tx_job_t *job = NULL;
        if (xQueueReceive(s_tx_queue,
                          &job,
                          pdMS_TO_TICKS(SDIO_FRAME_TX_POLL_MS)) != pdTRUE ||
            job == NULL) {
            continue;
        }

        if (!state_job_is_active(job->frame_id)) {
            ESP_LOGW(TAG,
                     "discarding stale queued frame=%" PRIu32 " after transport/state change",
                     job->frame_id);
            free_job(job);
            continue;
        }

        bool transport_fault = false;
        esp_err_t ret = send_jpeg(job, &transport_fault);
        if (ret == ESP_OK) {
            state_mark_frame_tx_complete(job->frame_id);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG,
                     "frame=%" PRIu32 " stopped by C6 control state",
                     job->frame_id);
        } else if (transport_fault) {
            state_mark_transport_down();
            ESP_LOGW(TAG,
                     "frame=%" PRIu32 " forwarding failed; reconnect will be retried",
                     job->frame_id);
        } else {
            state_release_frame_for_query(job->frame_id);
            ESP_LOGW(TAG,
                     "frame=%" PRIu32 " local send preparation failed (%s); C6 will be queried",
                     job->frame_id,
                     esp_err_to_name(ret));
        }
        free_job(job);
    }
}
#endif

esp_err_t sdio_frame_tx_init(void)
{
#if CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
    if (s_tx_queue != NULL) {
        return ESP_OK;
    }

    s_state_lock = xSemaphoreCreateMutex();
    if (s_state_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_tx_queue = xQueueCreate(SDIO_FRAME_TX_QUEUE_LENGTH, sizeof(sdio_frame_tx_job_t *));
    if (s_tx_queue == NULL) {
        vSemaphoreDelete(s_state_lock);
        s_state_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_next_frame_id = (uint32_t)esp_timer_get_time();
    s_next_time_request_id = s_next_frame_id ^ UINT32_C(0xa5c69e37);
    s_next_time_query_us = 0;
    sdio_time_clock_invalidate(&s_time_clock);
    if (xTaskCreate(sdio_frame_tx_worker,
                    "sdio_frame_tx",
                    SDIO_FRAME_TX_TASK_STACK_SIZE,
                    NULL,
                    SDIO_FRAME_TX_TASK_PRIORITY,
                    NULL) != pdPASS) {
        vQueueDelete(s_tx_queue);
        vSemaphoreDelete(s_state_lock);
        s_tx_queue = NULL;
        s_state_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool sdio_frame_tx_remote_ready(void)
{
#if CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
    bool ready = false;
    if (s_state_lock != NULL && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        /* A missing server-clock reply must not block image delivery.  The
         * capture-time helper returns zero until a valid server mapping is
         * available, and zero is the protocol's explicit UNKNOWN timestamp. */
        ready = s_transport_up && s_remote_state == SDIO_FRAME_CONTROL_READY &&
                s_active_frame_id == 0;
        xSemaphoreGive(s_state_lock);
    }
    return ready;
#else
    return false;
#endif
}

uint64_t sdio_frame_tx_capture_time_ms(void)
{
#if CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
    uint64_t epoch_ms = 0;
    const int64_t now_us = esp_timer_get_time();
    if (s_state_lock != NULL &&
        xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        epoch_ms = sdio_time_clock_now_ms(&s_time_clock, now_us);
        if (epoch_ms == 0 && s_time_clock.valid) {
            /* Expiration is a hard validity boundary. Keep the old numeric
             * anchor out of later captures and request a fresh server sample. */
            sdio_time_clock_invalidate(&s_time_clock);
            s_time_query_pending = s_transport_up &&
                                   s_remote_state == SDIO_FRAME_CONTROL_READY &&
                                   s_active_frame_id == 0 &&
                                   !s_time_query_outstanding;
            s_next_time_query_us = now_us;
        }
        xSemaphoreGive(s_state_lock);
    }
    return epoch_ms;
#else
    return 0;
#endif
}

esp_err_t sdio_frame_tx_submit_event(const uint8_t *jpeg,
                                     size_t jpeg_len,
                                     uint64_t detected_at_ms)
{
#if CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
    if (jpeg == NULL || jpeg_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (jpeg_len > SDIO_FRAME_MAX_JPEG_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_tx_queue == NULL || s_state_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (jpeg_len > SIZE_MAX - sizeof(sdio_frame_tx_job_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    sdio_frame_tx_job_t *job = malloc(sizeof(*job) + jpeg_len);
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }

    job->jpeg_len = jpeg_len;
    job->jpeg_crc32 = esp_crc32_le(0, jpeg, (uint32_t)jpeg_len);
    job->detected_at_ms = detected_at_ms;
    memcpy(job->jpeg, jpeg, jpeg_len);

    if (xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
        free_job(job);
        return ESP_ERR_TIMEOUT;
    }
    /* Sending is allowed while a server-clock query is outstanding.  If the
     * query later succeeds, subsequent captures carry server time; otherwise
     * this frame keeps detected_at_ms=0 and remains valid. */
    if (!s_transport_up || s_remote_state != SDIO_FRAME_CONTROL_READY ||
        s_active_frame_id != 0) {
        xSemaphoreGive(s_state_lock);
        free_job(job);
        return ESP_ERR_INVALID_STATE;
    }

    do {
        ++s_next_frame_id;
    } while (s_next_frame_id == 0);
    job->frame_id = s_next_frame_id;
    const uint32_t queued_frame_id = job->frame_id;
    s_active_frame_id = job->frame_id;
    s_active_frame_tx_complete = false;
    s_remote_state = SDIO_FRAME_CONTROL_BUSY;
    xSemaphoreGive(s_state_lock);

    if (xQueueSend(s_tx_queue, &job, 0) != pdTRUE) {
        if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
            if (s_active_frame_id == job->frame_id &&
                s_remote_state == SDIO_FRAME_CONTROL_BUSY) {
                s_active_frame_id = 0;
                s_active_frame_tx_complete = false;
                s_remote_state = SDIO_FRAME_CONTROL_READY;
            }
            xSemaphoreGive(s_state_lock);
        }
        free_job(job);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "reserved C6 READY window for frame=%" PRIu32, queued_frame_id);
    return ESP_OK;
#else
    (void)jpeg;
    (void)jpeg_len;
    (void)detected_at_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
