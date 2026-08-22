#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the ESP-Hosted custom-RPC frame/control/time receiver and its status
 * worker.
 *
 * ESP-Hosted must already be initialized.  This function also registers the
 * three custom handlers and the BLE Mesh source callbacks;
 * ble_mesh_image_source_init() may be called after this function returns.
 */
esp_err_t sdio_frame_receiver_init(void);

#ifdef __cplusplus
}
#endif
