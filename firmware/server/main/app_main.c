#include "mesh_image_gateway.h"

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#if CONFIG_SERVER_HTTP_ENABLE
#include "server_http_adapter.h"
#endif

#if CONFIG_SERVER_SERIAL_IMAGE_ENABLE
#include "server_serial_adapter.h"
#endif

#if CONFIG_SERVER_HTTP_ENABLE && CONFIG_SERVER_SERIAL_IMAGE_ENABLE
#error "SERVER_HTTP_ENABLE and SERVER_SERIAL_IMAGE_ENABLE are mutually exclusive"
#endif

#if !CONFIG_SERVER_HTTP_ENABLE && !CONFIG_SERVER_SERIAL_IMAGE_ENABLE
static const char *TAG = "mesh_server";

static void image_complete(const mesh_image_gateway_image_t *image,
                           void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG,
             "JPEG complete src=0x%04x event_ms=%llu source=%d bytes=%u",
             image->source_addr, (unsigned long long)image->event_time_ms,
             (int)image->time_source, (unsigned)image->jpeg_len);
}
#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

#if CONFIG_SERVER_SERIAL_IMAGE_ENABLE
    ESP_ERROR_CHECK(server_serial_adapter_init());
#elif CONFIG_SERVER_HTTP_ENABLE
    ESP_ERROR_CHECK(server_http_adapter_init());
#else
    ESP_ERROR_CHECK(mesh_image_gateway_register_image_callback(
        image_complete, NULL));
#endif
    ESP_ERROR_CHECK(mesh_image_gateway_init());
}
