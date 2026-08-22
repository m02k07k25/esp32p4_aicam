#ifndef BLE_MESH_IMAGE_SOURCE_H
#define BLE_MESH_IMAGE_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_mesh_image_source_ready_changed_cb_t)(bool ready,
                                                          void *user_ctx);

typedef void (*ble_mesh_image_source_frame_done_cb_t)(uint32_t p4_frame_id,
                                                       uint16_t ble_frame_id,
                                                       esp_err_t status,
                                                       void *user_ctx);

typedef void (*ble_mesh_image_source_time_done_cb_t)(
    uint32_t request_id,
    uint64_t client_tx_monotonic_us,
    bool available,
    uint64_t server_rx_unix_ms,
    uint64_t server_tx_unix_ms,
    esp_err_t status,
    void *user_ctx);

typedef struct {
    ble_mesh_image_source_ready_changed_cb_t ready_changed;
    ble_mesh_image_source_frame_done_cb_t frame_done;
    ble_mesh_image_source_time_done_cb_t time_done;
} ble_mesh_image_source_callbacks_t;

/**
 * Register application callbacks. This may be called before init.
 *
 * Callbacks execute in a BLE Mesh callback task (ready_changed) or the image
 * worker task (frame_done), so they must not block indefinitely.
 */
esp_err_t ble_mesh_image_source_register_callbacks(
    const ble_mesh_image_source_callbacks_t *callbacks,
    void *user_ctx);

/** Initialize the local Bluetooth host, BLE Mesh node, and image worker. */
esp_err_t ble_mesh_image_source_init(void);

/**
 * Queue one JPEG for BLE Mesh transfer without copying it.
 *
 * On ESP_OK, jpeg must remain readable and unchanged until frame_done is
 * invoked. The buffer may be reused as soon as frame_done is entered. Only
 * one queued or active JPEG is accepted at a time.
 *
 * A frame_done status of ESP_OK means the configured Gateway returned the
 * protocol COMPLETE response after validating the reassembled JPEG. The
 * source retains jpeg until that callback is entered.
 *
 * ble_frame_id is optional and receives the assigned on-air frame ID.
 */
esp_err_t ble_mesh_image_source_submit(const uint8_t *jpeg,
                                       size_t len,
                                       uint32_t p4_frame_id,
                                       uint64_t detected_at_ms,
                                       uint32_t jpeg_crc32,
                                       uint16_t *ble_frame_id);

/**
 * Queue one server-clock request on the same worker used by JPEG transfers.
 *
 * Exactly one image or clock operation may be queued/active. ESP_OK means the
 * request was queued; ESP_ERR_INVALID_STATE means Mesh is not READY;
 * ESP_ERR_NOT_FINISHED means another operation owns the worker. The completion
 * callback reports an authenticated server sample, an explicit unavailable
 * response, or a transport/protocol failure.
 */
esp_err_t ble_mesh_image_source_request_time(
    uint32_t request_id, uint64_t client_tx_monotonic_us);

/**
 * True only when provisioned, AppKey-bound, transport-healthy, and configured
 * with a unicast publication destination using the bound AppKey.
 */
bool ble_mesh_image_source_is_ready(void);

/** True while one submitted JPEG is queued or being transmitted/repaired. */
bool ble_mesh_image_source_is_busy(void);

/*
 * Optional weak hooks. Applications may provide strong definitions instead
 * of (or in addition to) registering callbacks above.
 */
void ble_mesh_image_source_ready_changed(bool ready);
void ble_mesh_image_source_frame_done(uint32_t p4_frame_id,
                                      uint16_t ble_frame_id,
                                      esp_err_t status);
void ble_mesh_image_source_time_done(uint32_t request_id,
                                     uint64_t client_tx_monotonic_us,
                                     bool available,
                                     uint64_t server_rx_unix_ms,
                                     uint64_t server_tx_unix_ms,
                                     esp_err_t status);

#ifdef __cplusplus
}
#endif

#endif /* BLE_MESH_IMAGE_SOURCE_H */
