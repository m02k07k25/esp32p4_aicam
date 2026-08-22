#include "server_http_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "ble_mesh_image_protocol.h"
#include "mesh_image_gateway.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "server_http"

static SemaphoreHandle_t s_latest_mutex;
static uint8_t *s_latest_jpeg;
static size_t s_latest_len;
static uint16_t s_latest_source;
static uint64_t s_latest_event_ms;
static server_time_source_t s_latest_time_source = SERVER_TIME_UNKNOWN;
static bool s_http_started;

static const char *time_source_name(server_time_source_t source)
{
    switch (source) {
    case SERVER_TIME_P4_DETECTED:
        return "p4_detected";
    case SERVER_TIME_RX_ESTIMATE:
        return "rx_estimate";
    default:
        return "unknown";
    }
}

static esp_err_t send_service_unavailable(httpd_req_t *request,
                                           const char *message)
{
    httpd_resp_set_status(request, "503 Service Unavailable");
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_send(request, message, HTTPD_RESP_USE_STRLEN);
}

static void image_complete(const server_image_t *image, void *user_ctx)
{
    (void)user_ctx;
    if (image == NULL || image->jpeg == NULL ||
        image->jpeg_len > BLE_MESH_IMAGE_MAX_BYTES ||
        s_latest_jpeg == NULL) {
        return;
    }
    xSemaphoreTake(s_latest_mutex, portMAX_DELAY);
    memcpy(s_latest_jpeg, image->jpeg, image->jpeg_len);
    s_latest_len = image->jpeg_len;
    s_latest_source = image->source_addr;
    s_latest_event_ms = image->event_time_ms;
    s_latest_time_source = image->time_source;
    xSemaphoreGive(s_latest_mutex);
    ESP_LOGI(TAG,
             "latest JPEG id=%u src=0x%04x event_ms=%llu "
             "time_source=%s bytes=%u",
             image->device_id, image->source_addr,
             (unsigned long long)image->event_time_ms,
             time_source_name(image->time_source),
             (unsigned)image->jpeg_len);
}

static esp_err_t latest_jpeg_handler(httpd_req_t *request)
{
    if (!mesh_image_gateway_try_begin_idle_work()) {
        httpd_resp_set_hdr(request, "Retry-After", "1");
        return send_service_unavailable(request,
                                        "Mesh radio window is busy");
    }
    xSemaphoreTake(s_latest_mutex, portMAX_DELAY);
    size_t len = s_latest_len;
    uint8_t *copy = len == 0U ? NULL : malloc(len);
    if (copy != NULL) {
        memcpy(copy, s_latest_jpeg, len);
    }
    xSemaphoreGive(s_latest_mutex);

    if (len == 0U) {
        esp_err_t err = httpd_resp_send_err(request, HTTPD_404_NOT_FOUND,
                                            "no complete image");
        mesh_image_gateway_end_idle_work();
        return err;
    }
    if (copy == NULL) {
        esp_err_t err = send_service_unavailable(
            request, "snapshot allocation failed");
        mesh_image_gateway_end_idle_work();
        return err;
    }
    httpd_resp_set_type(request, "image/jpeg");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(request, (const char *)copy, len);
    free(copy);
    mesh_image_gateway_end_idle_work();
    return err;
}

static esp_err_t latest_json_handler(httpd_req_t *request)
{
    uint16_t source;
    uint64_t event_ms;
    size_t len;
    server_time_source_t time_source;
    xSemaphoreTake(s_latest_mutex, portMAX_DELAY);
    source = s_latest_source;
    event_ms = s_latest_event_ms;
    len = s_latest_len;
    time_source = s_latest_time_source;
    xSemaphoreGive(s_latest_mutex);

    if (len == 0U) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND,
                                   "no complete image");
    }
    char json[192];
    int written = snprintf(
        json, sizeof(json),
        "{\"device_id\":%u,\"source_addr\":%u,\"event_time_ms\":%llu,"
        "\"time_source\":\"%s\",\"jpeg_len\":%u}",
        mesh_image_gateway_device_id_from_addr(source), source,
        (unsigned long long)event_ms,
        time_source_name(time_source), (unsigned)len);
    if (written < 0 || (size_t)written >= sizeof(json)) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, json, written);
}

static esp_err_t start_http(void)
{
    if (s_http_started) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_SERVER_HTTP_PORT;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t jpeg_uri = {
        .uri = "/latest.jpg",
        .method = HTTP_GET,
        .handler = latest_jpeg_handler,
    };
    const httpd_uri_t json_uri = {
        .uri = "/latest.json",
        .method = HTTP_GET,
        .handler = latest_json_handler,
    };
    err = httpd_register_uri_handler(server, &jpeg_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &json_uri);
    }
    if (err == ESP_OK) {
        s_http_started = true;
        ESP_LOGI(TAG, "HTTP listening on port %d", CONFIG_SERVER_HTTP_PORT);
    }
    return err;
}

esp_err_t server_http_adapter_init(void)
{
    s_latest_mutex = xSemaphoreCreateMutex();
    if (s_latest_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* One fixed allocation avoids a second 30 KiB BSS slot and heap churn. */
    s_latest_jpeg = malloc(BLE_MESH_IMAGE_MAX_BYTES);
    if (s_latest_jpeg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(mesh_image_gateway_register_image_callback(
        image_complete, NULL));
    return start_http();
}
