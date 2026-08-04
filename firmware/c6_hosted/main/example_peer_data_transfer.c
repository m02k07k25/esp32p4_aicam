/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#ifdef CONFIG_EXAMPLE_PEER_DATA_TRANSFER

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdio_frame_protocol.h"
#include "slave_control.h"

static const char *TAG = "c6_frame_http";

typedef struct {
    uint8_t *jpeg;
    size_t jpeg_len;
    uint32_t frame_id;
    uint16_t chunk_count;
    int32_t class_id;
    float score;
    float inference_ms;
    float total_ms;
    int64_t received_at_us;
    char class_name[SDIO_FRAME_CLASS_NAME_MAX];
} latest_frame_t;

static SemaphoreHandle_t s_latest_lock;
static latest_frame_t s_latest;

/* Only the ESP-Hosted custom RPC callback accesses the assembly state. */
static uint8_t *s_assembly_jpeg;
static uint32_t s_assembly_frame_id;
static uint32_t s_assembly_jpeg_size;
static uint32_t s_assembly_bytes;
static uint16_t s_assembly_chunk_count;
static uint16_t s_assembly_next_chunk;

static void reset_assembly(void)
{
    free(s_assembly_jpeg);
    s_assembly_jpeg = NULL;
    s_assembly_frame_id = 0;
    s_assembly_jpeg_size = 0;
    s_assembly_bytes = 0;
    s_assembly_chunk_count = 0;
    s_assembly_next_chunk = 0;
}

static bool validate_chunk_header(const sdio_frame_chunk_header_t *header, size_t data_len)
{
    if (header->magic != SDIO_FRAME_MAGIC || header->version != SDIO_FRAME_VERSION ||
        header->header_size != sizeof(*header)) {
        return false;
    }
    if (header->chunk_count == 0 || header->chunk_index >= header->chunk_count ||
        header->chunk_size > SDIO_FRAME_CHUNK_DATA_MAX) {
        return false;
    }
    if (header->jpeg_size == 0 || header->jpeg_size > CONFIG_C6_FRAME_MAX_JPEG_SIZE) {
        return false;
    }
    if (data_len != (size_t)header->header_size + header->chunk_size) {
        return false;
    }
    if (header->chunk_offset > header->jpeg_size ||
        header->chunk_size > header->jpeg_size - header->chunk_offset) {
        return false;
    }

    const bool is_first = header->chunk_index == 0;
    const bool is_last = header->chunk_index == header->chunk_count - 1;
    if (is_first != ((header->flags & SDIO_FRAME_FLAG_FIRST_CHUNK) != 0) ||
        is_last != ((header->flags & SDIO_FRAME_FLAG_LAST_CHUNK) != 0)) {
        return false;
    }

    return true;
}

static bool start_assembly(const sdio_frame_chunk_header_t *header)
{
    reset_assembly();

    s_assembly_jpeg = malloc(header->jpeg_size);
    if (s_assembly_jpeg == NULL) {
        ESP_LOGE(TAG, "Unable to allocate %" PRIu32 " bytes for frame", header->jpeg_size);
        return false;
    }

    s_assembly_frame_id = header->frame_id;
    s_assembly_jpeg_size = header->jpeg_size;
    s_assembly_chunk_count = header->chunk_count;
    return true;
}

static void publish_assembly(const sdio_frame_chunk_header_t *header)
{
    uint8_t *old_jpeg = NULL;

    /* This runs in the ESP-Hosted RX task. Never wait behind a slow HTTP client. */
    if (xSemaphoreTake(s_latest_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Latest frame is busy; dropping completed frame=%" PRIu32,
                 s_assembly_frame_id);
        reset_assembly();
        return;
    }

    old_jpeg = s_latest.jpeg;
    s_latest.jpeg = s_assembly_jpeg;
    s_latest.jpeg_len = s_assembly_jpeg_size;
    s_latest.frame_id = s_assembly_frame_id;
    s_latest.chunk_count = s_assembly_chunk_count;
    s_latest.class_id = header->class_id;
    s_latest.score = header->score;
    s_latest.inference_ms = header->inference_ms;
    s_latest.total_ms = header->total_ms;
    s_latest.received_at_us = esp_timer_get_time();
    memcpy(s_latest.class_name, header->class_name, sizeof(s_latest.class_name));
    s_latest.class_name[sizeof(s_latest.class_name) - 1] = '\0';

    s_assembly_jpeg = NULL;
    s_assembly_jpeg_size = 0;
    s_assembly_bytes = 0;
    s_assembly_chunk_count = 0;
    s_assembly_next_chunk = 0;

    xSemaphoreGive(s_latest_lock);
    free(old_jpeg);

    ESP_LOGI(TAG,
             "Published frame=%" PRIu32 " jpeg=%u B chunks=%u class=%ld (%s) score=%.4f",
             s_latest.frame_id,
             (unsigned int)s_latest.jpeg_len,
             (unsigned int)s_latest.chunk_count,
             (long)s_latest.class_id,
             s_latest.class_name,
             s_latest.score);
}

static void frame_chunk_callback(uint32_t msg_id, const uint8_t *data, size_t data_len)
{
    sdio_frame_chunk_header_t header;

    if (msg_id != SDIO_FRAME_MSG_ID || data == NULL || data_len < sizeof(header)) {
        return;
    }

    /* The RPC payload is byte-aligned; copy before reading packed numeric fields. */
    memcpy(&header, data, sizeof(header));
    if (!validate_chunk_header(&header, data_len)) {
        ESP_LOGW(TAG, "Rejected invalid frame chunk (%u bytes)", (unsigned int)data_len);
        reset_assembly();
        return;
    }

    if (header.chunk_index == 0) {
        if (!start_assembly(&header)) {
            return;
        }
    }

    if (s_assembly_jpeg == NULL || header.frame_id != s_assembly_frame_id ||
        header.jpeg_size != s_assembly_jpeg_size ||
        header.chunk_count != s_assembly_chunk_count ||
        header.chunk_index != s_assembly_next_chunk ||
        header.chunk_offset != s_assembly_bytes) {
        ESP_LOGW(TAG,
                 "Out-of-order frame=%" PRIu32 " chunk=%u/%u; dropping partial frame",
                 header.frame_id,
                 (unsigned int)header.chunk_index + 1,
                 (unsigned int)header.chunk_count);
        reset_assembly();
        return;
    }

    memcpy(s_assembly_jpeg + header.chunk_offset, data + header.header_size, header.chunk_size);
    s_assembly_bytes += header.chunk_size;
    s_assembly_next_chunk++;

    if ((header.flags & SDIO_FRAME_FLAG_LAST_CHUNK) != 0) {
        if (s_assembly_bytes != s_assembly_jpeg_size ||
            s_assembly_next_chunk != s_assembly_chunk_count) {
            ESP_LOGW(TAG, "Incomplete frame=%" PRIu32 "; dropping", header.frame_id);
            reset_assembly();
            return;
        }
        publish_assembly(&header);
    }
}

static esp_err_t index_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32-C6 received frame</title>"
        "<style>body{font-family:sans-serif;background:#181a1b;color:#eee;margin:24px}"
        "img{max-width:100%;height:auto;border:1px solid #555}pre{white-space:pre-wrap}</style>"
        "</head><body><h1>ESP32-C6 received frame</h1>"
        "<img id=\"frame\" alt=\"No frame received yet\"><pre id=\"status\">Waiting...</pre>"
        "<script>async function refresh(){const t=Date.now();"
        "document.getElementById('frame').src='/received.jpg?t='+t;"
        "try{const r=await fetch('/status?t='+t);"
        "document.getElementById('status').textContent=JSON.stringify(await r.json(),null,2);"
        "}catch(e){document.getElementById('status').textContent=String(e);}}"
        "refresh();setInterval(refresh,1500);</script></body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t received_jpeg_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(s_latest_lock, portMAX_DELAY) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "frame lock failed");
    }

    if (s_latest.jpeg == NULL || s_latest.jpeg_len == 0) {
        xSemaphoreGive(s_latest_lock);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "No complete frame received from P4 yet\n");
    }

    char frame_id[16];
    char class_id[16];
    char score[24];
    char inference_ms[24];
    char total_ms[24];
    snprintf(frame_id, sizeof(frame_id), "%" PRIu32, s_latest.frame_id);
    snprintf(class_id, sizeof(class_id), "%ld", (long)s_latest.class_id);
    snprintf(score, sizeof(score), "%.4f", s_latest.score);
    snprintf(inference_ms, sizeof(inference_ms), "%.2f", s_latest.inference_ms);
    snprintf(total_ms, sizeof(total_ms), "%.2f", s_latest.total_ms);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=received.jpg");
    httpd_resp_set_hdr(req, "X-Frame-Id", frame_id);
    httpd_resp_set_hdr(req, "X-Class-Index", class_id);
    httpd_resp_set_hdr(req, "X-Class-Label", s_latest.class_name);
    httpd_resp_set_hdr(req, "X-Class-Score", score);
    httpd_resp_set_hdr(req, "X-Inference-Time-Ms", inference_ms);
    httpd_resp_set_hdr(req, "X-Inference-Total-Ms", total_ms);

    esp_err_t ret = httpd_resp_send(req, (const char *)s_latest.jpeg, s_latest.jpeg_len);
    xSemaphoreGive(s_latest_lock);
    return ret;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char json[384];

    if (xSemaphoreTake(s_latest_lock, portMAX_DELAY) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "frame lock failed");
    }

    if (s_latest.jpeg == NULL) {
        snprintf(json, sizeof(json), "{\"ready\":false}");
    } else {
        int64_t age_ms = (esp_timer_get_time() - s_latest.received_at_us) / 1000;
        snprintf(json,
                 sizeof(json),
                 "{\"ready\":true,\"frame_id\":%" PRIu32
                 ",\"jpeg_bytes\":%u,\"chunks\":%u,\"class_id\":%ld,"
                 "\"class_name\":\"%s\",\"score\":%.4f,\"inference_ms\":%.2f,"
                 "\"p4_total_ms\":%.2f,\"age_ms\":%lld}",
                 s_latest.frame_id,
                 (unsigned int)s_latest.jpeg_len,
                 (unsigned int)s_latest.chunk_count,
                 (long)s_latest.class_id,
                 s_latest.class_name,
                 s_latest.score,
                 s_latest.inference_ms,
                 s_latest.total_ms,
                 (long long)age_ms);
    }

    xSemaphoreGive(s_latest_lock);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t start_frame_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    config.server_port = CONFIG_C6_FRAME_HTTP_PORT;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        return ret;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    const httpd_uri_t jpeg_uri = {
        .uri = "/received.jpg",
        .method = HTTP_GET,
        .handler = received_jpeg_handler,
    };
    const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };

    if ((ret = httpd_register_uri_handler(server, &index_uri)) != ESP_OK ||
        (ret = httpd_register_uri_handler(server, &jpeg_uri)) != ESP_OK ||
        (ret = httpd_register_uri_handler(server, &status_uri)) != ESP_OK) {
        httpd_stop(server);
        return ret;
    }

    ESP_LOGI(TAG,
             "C6 HTTP server started on port %d: /, /received.jpg, /status",
             CONFIG_C6_FRAME_HTTP_PORT);
    return ESP_OK;
}

esp_err_t example_peer_data_transfer_init(void)
{
    s_latest_lock = xSemaphoreCreateMutex();
    if (s_latest_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_hosted_register_custom_callback(SDIO_FRAME_MSG_ID, frame_chunk_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register frame callback: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Registered P4 frame callback, message ID=0x%08" PRIx32, SDIO_FRAME_MSG_ID);
    ret = start_frame_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start C6 HTTP server: %s", esp_err_to_name(ret));
    }
    return ret;
}

#endif /* CONFIG_EXAMPLE_PEER_DATA_TRANSFER */
