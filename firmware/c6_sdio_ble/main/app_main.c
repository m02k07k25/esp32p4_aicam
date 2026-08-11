/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_mesh_image_source.h"
#include "esp_err.h"
#include "esp_hosted_coprocessor.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdio_frame_receiver.h"

static const char *TAG = "c6_sdio_ble";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Bring up the native C6 SDIO slave before registering custom RPC IDs. */
    ESP_ERROR_CHECK(esp_hosted_coprocessor_init());
    ESP_ERROR_CHECK(sdio_frame_receiver_init());

    /*
     * Until provisioning/AppKey binding completes, QUERY receives NOT_READY.
     * The BLE module's ready_changed callback makes the transition visible to
     * the P4 without ever transmitting from a Hosted RX callback.
     */
    ret = ble_mesh_image_source_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "BLE Mesh initialization failed; SDIO remains NOT_READY: %s",
                 esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "ESP-Hosted SDIO + local BLE Mesh initialized");
}
