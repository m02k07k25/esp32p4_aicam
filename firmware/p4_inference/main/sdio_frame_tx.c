#include "sdio_frame_tx.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
#include "esp_hosted.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "sdio_frame_tx";

#define SDIO_FRAME_TX_QUEUE_LENGTH 1
#define SDIO_FRAME_TX_TASK_STACK_SIZE 4096
#define SDIO_FRAME_TX_TASK_PRIORITY 4

typedef struct {
    size_t jpeg_len;
    infer_result_t result;
    float total_ms;
    char class_name[SDIO_FRAME_CLASS_NAME_MAX];
    uint8_t jpeg[];
} sdio_frame_tx_job_t;

static QueueHandle_t s_tx_queue;
static volatile bool s_tx_disabled;

static void copy_class_name(char dest[SDIO_FRAME_CLASS_NAME_MAX], const char *src)
{
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }

    snprintf(dest, SDIO_FRAME_CLASS_NAME_MAX, "%s", src);
}
#endif

static esp_err_t sdio_frame_tx_send_classification(const uint8_t *jpeg,
                                                   size_t jpeg_len,
                                                   const infer_result_t *result,
                                                   float total_ms)
{
#if CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
    if (jpeg == NULL || jpeg_len == 0 || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (jpeg_len > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((jpeg_len + SDIO_FRAME_CHUNK_DATA_MAX - 1) / SDIO_FRAME_CHUNK_DATA_MAX > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t header_size = sizeof(sdio_frame_chunk_header_t);
    const size_t tx_buf_size = header_size + SDIO_FRAME_CHUNK_DATA_MAX;
    uint8_t *tx_buf = (uint8_t *)malloc(tx_buf_size);
    if (tx_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const uint16_t chunk_count =
        (uint16_t)((jpeg_len + SDIO_FRAME_CHUNK_DATA_MAX - 1) / SDIO_FRAME_CHUNK_DATA_MAX);
    const uint32_t frame_id = (uint32_t)esp_timer_get_time();
    esp_err_t ret = ESP_OK;

    for (uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const size_t offset = (size_t)chunk_index * SDIO_FRAME_CHUNK_DATA_MAX;
        const size_t chunk_size = (jpeg_len - offset > SDIO_FRAME_CHUNK_DATA_MAX)
                                      ? SDIO_FRAME_CHUNK_DATA_MAX
                                      : jpeg_len - offset;

        sdio_frame_chunk_header_t header = {
            .magic = SDIO_FRAME_MAGIC,
            .version = SDIO_FRAME_VERSION,
            .header_size = (uint16_t)header_size,
            .frame_id = frame_id,
            .chunk_index = chunk_index,
            .chunk_count = chunk_count,
            .flags = 0,
            .chunk_offset = (uint32_t)offset,
            .chunk_size = (uint32_t)chunk_size,
            .jpeg_size = (uint32_t)jpeg_len,
            .class_id = result->class_id,
            .score = result->score,
            .inference_ms = result->inference_ms,
            .total_ms = total_ms,
        };
        if (chunk_index == 0) {
            header.flags |= SDIO_FRAME_FLAG_FIRST_CHUNK;
        }
        if (chunk_index == chunk_count - 1) {
            header.flags |= SDIO_FRAME_FLAG_LAST_CHUNK;
        }
        copy_class_name(header.class_name, result->class_name);

        memcpy(tx_buf, &header, header_size);
        memcpy(tx_buf + header_size, jpeg + offset, chunk_size);

        ret = esp_hosted_send_custom_data(SDIO_FRAME_MSG_ID, tx_buf, header_size + chunk_size);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "failed to send frame=%" PRIu32 " chunk=%u/%u over SDIO: %s",
                     frame_id,
                     (unsigned int)chunk_index + 1,
                     (unsigned int)chunk_count,
                     esp_err_to_name(ret));
            break;
        }
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "sent frame=%" PRIu32 " jpeg=%u B chunks=%u class=%d score=%.4f",
                 frame_id,
                 (unsigned int)jpeg_len,
                 (unsigned int)chunk_count,
                 result->class_id,
                 result->score);
    }

    free(tx_buf);
    return ret;
#else
    (void)jpeg;
    (void)jpeg_len;
    (void)result;
    (void)total_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#if CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
static void sdio_frame_tx_free_job(sdio_frame_tx_job_t *job)
{
    free(job);
}

static void sdio_frame_tx_worker(void *arg)
{
    (void)arg;

    while (true) {
        sdio_frame_tx_job_t *job = NULL;
        if (xQueueReceive(s_tx_queue, &job, portMAX_DELAY) != pdTRUE || job == NULL) {
            continue;
        }

        if (!s_tx_disabled) {
            esp_err_t ret = sdio_frame_tx_send_classification(
                job->jpeg, job->jpeg_len, &job->result, job->total_ms);
            if (ret != ESP_OK) {
                s_tx_disabled = true;
                ESP_LOGW(TAG,
                         "SDIO unavailable (%s); disabling frame forwarding until reboot",
                         esp_err_to_name(ret));
            }
        }

        sdio_frame_tx_free_job(job);

        if (s_tx_disabled) {
            while (xQueueReceive(s_tx_queue, &job, 0) == pdTRUE) {
                sdio_frame_tx_free_job(job);
            }
        }
    }
}
#endif

esp_err_t sdio_frame_tx_init(void)
{
#if CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
    if (s_tx_queue != NULL) {
        return ESP_OK;
    }

    s_tx_queue = xQueueCreate(SDIO_FRAME_TX_QUEUE_LENGTH, sizeof(sdio_frame_tx_job_t *));
    if (s_tx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(sdio_frame_tx_worker,
                    "sdio_frame_tx",
                    SDIO_FRAME_TX_TASK_STACK_SIZE,
                    NULL,
                    SDIO_FRAME_TX_TASK_PRIORITY,
                    NULL) != pdPASS) {
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sdio_frame_tx_submit_classification(const uint8_t *jpeg,
                                              size_t jpeg_len,
                                              const infer_result_t *result,
                                              float total_ms)
{
#if CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
    if (jpeg == NULL || jpeg_len == 0 || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tx_disabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (jpeg_len > SIZE_MAX - sizeof(sdio_frame_tx_job_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    sdio_frame_tx_job_t *job = malloc(sizeof(*job) + jpeg_len);
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }

    job->jpeg_len = jpeg_len;
    job->result = *result;
    job->total_ms = total_ms;
    copy_class_name(job->class_name, result->class_name);
    job->result.class_name = job->class_name;
    memcpy(job->jpeg, jpeg, jpeg_len);

    if (xQueueSend(s_tx_queue, &job, 0) != pdTRUE) {
        sdio_frame_tx_free_job(job);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
#else
    (void)jpeg;
    (void)jpeg_len;
    (void)result;
    (void)total_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
