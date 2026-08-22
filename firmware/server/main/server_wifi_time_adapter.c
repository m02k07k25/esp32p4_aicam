#include "server_wifi_time_adapter.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "sdkconfig.h"

#include "mesh_image_gateway.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#define TAG "server_wifi_time"

#define CLOCK_VALID_AFTER_UNIX_S UINT64_C(1704067200) /* 2024-01-01 UTC */
#define RECONNECT_STEP_COUNT     5U

static const uint32_t s_reconnect_delay_ms[RECONNECT_STEP_COUNT] = {
    1000U, 2000U, 4000U, 8000U, 10000U,
};

static TimerHandle_t s_reconnect_timer;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_clock_synchronized;
static int64_t s_last_sync_monotonic_us;
static bool s_has_ip;
static uint8_t s_reconnect_step;

static bool wall_clock_now(uint64_t *unix_ms, void *user_ctx)
{
    (void)user_ctx;
    if (unix_ms == NULL) {
        return false;
    }

    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    bool synchronized = s_clock_synchronized &&
                        now_us >= s_last_sync_monotonic_us &&
                        (uint64_t)(now_us - s_last_sync_monotonic_us) <=
                            (uint64_t)CONFIG_SERVER_SNTP_MAX_AGE_MS *
                                UINT64_C(1000);
    portEXIT_CRITICAL(&s_state_lock);
    if (!synchronized) {
        return false;
    }

    struct timeval value;
    if (gettimeofday(&value, NULL) != 0 || value.tv_sec < 0 ||
        (uint64_t)value.tv_sec < CLOCK_VALID_AFTER_UNIX_S) {
        return false;
    }
    *unix_ms = (uint64_t)value.tv_sec * UINT64_C(1000) +
               (uint64_t)value.tv_usec / UINT64_C(1000);
    return true;
}

static void time_synchronized(struct timeval *value)
{
    if (value == NULL || value->tv_sec < 0 ||
        (uint64_t)value->tv_sec < CLOCK_VALID_AFTER_UNIX_S) {
        ESP_LOGW(TAG, "ignored invalid SNTP time");
        return;
    }

    uint64_t unix_ms = (uint64_t)value->tv_sec * UINT64_C(1000) +
                       (uint64_t)value->tv_usec / UINT64_C(1000);
    portENTER_CRITICAL(&s_state_lock);
    s_clock_synchronized = true;
    s_last_sync_monotonic_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "SNTP synchronized: epoch_ms=%llu",
             (unsigned long long)unix_ms);
}

static void reconnect_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    portENTER_CRITICAL(&s_state_lock);
    bool should_connect = !s_has_ip;
    portEXIT_CRITICAL(&s_state_lock);
    if (should_connect) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi reconnect start failed: %s",
                     esp_err_to_name(err));
        }
    }
}

static void schedule_reconnect(void)
{
    portENTER_CRITICAL(&s_state_lock);
    size_t step = s_reconnect_step;
    if (s_reconnect_step + 1U < RECONNECT_STEP_COUNT) {
        ++s_reconnect_step;
    }
    portEXIT_CRITICAL(&s_state_lock);

    TickType_t delay = pdMS_TO_TICKS(s_reconnect_delay_ms[step]);
    if (delay == 0U) {
        delay = 1U;
    }
    if (xTimerChangePeriod(s_reconnect_timer, delay, 0) != pdPASS) {
        ESP_LOGE(TAG, "could not schedule Wi-Fi reconnect");
        return;
    }
    ESP_LOGW(TAG, "Wi-Fi reconnect scheduled in %lu ms",
             (unsigned long)s_reconnect_delay_ms[step]);
}

static void network_event(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        return;
    }
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        portENTER_CRITICAL(&s_state_lock);
        s_has_ip = false;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGW(TAG, "Wi-Fi disconnected reason=%u",
                 disconnected == NULL ? 0U : disconnected->reason);
        schedule_reconnect();
        return;
    }
    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = event_data;
        portENTER_CRITICAL(&s_state_lock);
        s_has_ip = true;
        s_reconnect_step = 0U;
        portEXIT_CRITICAL(&s_state_lock);
        (void)xTimerStop(s_reconnect_timer, 0);
        if (got_ip != NULL) {
            ESP_LOGI(TAG, "Wi-Fi got IPv4 " IPSTR,
                     IP2STR(&got_ip->ip_info.ip));
        }
        esp_err_t err = esp_netif_sntp_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SNTP start/restart failed: %s",
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "SNTP polling '%s'", CONFIG_SERVER_SNTP_SERVER);
        }
    }
}

esp_err_t server_wifi_time_adapter_init(void)
{
    if (CONFIG_SERVER_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "SERVER_WIFI_SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }

    s_reconnect_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(1000),
                                     pdFALSE, NULL,
                                     reconnect_timer_callback);
    if (s_reconnect_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = mesh_image_gateway_set_time_provider(wall_clock_now, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_netif_init();
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

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(
        CONFIG_SERVER_SNTP_SERVER);
    sntp.start = false;
    sntp.wait_for_sync = false;
    sntp.sync_cb = time_synchronized;
    err = esp_netif_sntp_init(&sntp);
    if (err != ESP_OK) {
        return err;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                         network_event, NULL);
    }
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
        err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Wi-Fi STA/SNTP initialized independently of image export");
    }
    return err;
}
