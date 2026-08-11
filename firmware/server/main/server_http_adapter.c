#include "server_http_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "sdkconfig.h"

#include "ble_mesh_image_protocol.h"
#include "mesh_image_gateway.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "server_http"

static SemaphoreHandle_t s_latest_mutex;
static uint8_t *s_latest_jpeg;
static size_t s_latest_len;
static uint16_t s_latest_source;
static uint64_t s_latest_event_ms;
static server_time_source_t s_latest_time_source = SERVER_TIME_UNKNOWN;
static bool s_sntp_started;
static bool s_sntp_finished;
static bool s_sntp_task_started;
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

static bool wall_clock_now(uint64_t *unix_ms, void *user_ctx)
{
    (void)user_ctx;
    struct timeval value;
    if (unix_ms == NULL || gettimeofday(&value, NULL) != 0 ||
        value.tv_sec < 1577836800) {
        return false;
    }
    *unix_ms = (uint64_t)value.tv_sec * 1000U +
               (uint64_t)value.tv_usec / 1000U;
    return true;
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
             "latest JPEG src=0x%04x event_ms=%llu time_source=%s bytes=%u",
             image->source_addr, (unsigned long long)image->event_time_ms,
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
        "{\"source_addr\":%u,\"event_time_ms\":%llu,"
        "\"time_source\":\"%s\",\"jpeg_len\":%u}",
        source, (unsigned long long)event_ms,
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

static esp_err_t start_sntp(void)
{
    if (s_sntp_started) {
        return ESP_OK;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(
        CONFIG_SERVER_SNTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_sntp_started = true;
    }
    return err;
}

static void one_shot_sntp_task(void *arg)
{
    (void)arg;
    while (!mesh_image_gateway_try_begin_idle_work()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (start_sntp() == ESP_OK) {
        esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "one-shot SNTP synchronization complete");
        } else {
            ESP_LOGW(TAG, "one-shot SNTP synchronization timed out");
        }
        esp_netif_sntp_deinit();
        s_sntp_started = false;
        s_sntp_finished = true;
    }
    mesh_image_gateway_end_idle_work();
    s_sntp_task_started = false;
    vTaskDelete(NULL);
}

static void network_event(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(start_http());
        if (!s_sntp_finished && !s_sntp_task_started) {
            s_sntp_task_started = xTaskCreate(
                one_shot_sntp_task, "server_sntp", 3072U,
                NULL, 3U, NULL) == pdPASS;
        }
    }
}

esp_err_t server_http_adapter_init(void)
{
    if (CONFIG_SERVER_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "SERVER_WIFI_SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }
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
    ESP_ERROR_CHECK(mesh_image_gateway_set_time_provider(
        wall_clock_now, NULL));

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     network_event, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         network_event, NULL);
    }
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi = {0};
    strlcpy((char *)wifi.sta.ssid, CONFIG_SERVER_WIFI_SSID,
            sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password, CONFIG_SERVER_WIFI_PASSWORD,
            sizeof(wifi.sta.password));
    wifi.sta.threshold.authmode = WIFI_AUTH_OPEN;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    return err;
}
