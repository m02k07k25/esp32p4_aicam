#ifndef MESH_IMAGE_GATEWAY_H
#define MESH_IMAGE_GATEWAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVER_TIME_P4_DETECTED,
    SERVER_TIME_RX_ESTIMATE,
    SERVER_TIME_UNKNOWN,
} server_time_source_t;

typedef struct {
    uint16_t source_addr;
    uint16_t device_id;
    uint64_t event_time_ms;
    server_time_source_t time_source;
    const uint8_t *jpeg;
    size_t jpeg_len;
} server_image_t;

typedef server_time_source_t mesh_image_gateway_time_source_t;
typedef server_image_t mesh_image_gateway_image_t;

/* jpeg remains immutable and valid only until the callback returns. */
typedef void (*server_image_callback_t)(const server_image_t *image,
                                        void *user_ctx);
typedef server_image_callback_t mesh_image_gateway_image_cb_t;

/*
 * Optional fast, nonblocking wall-clock source sampled on a timestamp-less
 * OPEN. Its first valid value becomes SERVER_TIME_RX_ESTIMATE; it is never
 * transmitted back over Mesh.
 */
typedef bool (*server_time_provider_t)(uint64_t *unix_ms, void *user_ctx);
typedef server_time_provider_t mesh_image_gateway_time_provider_t;

esp_err_t mesh_image_gateway_register_image_callback(
    mesh_image_gateway_image_cb_t callback, void *user_ctx);

esp_err_t mesh_image_gateway_set_time_provider(
    mesh_image_gateway_time_provider_t provider, void *user_ctx);

/* True only while the single JPEG reassembly slot is active. */
bool mesh_image_gateway_is_receiving(void);

/*
 * C6 installation IDs are compiled into the Device UUID.  The Provisioner
 * assigns the deterministic primary address (device_id + 1), with 0x0001
 * reserved for this Gateway.  Zero means that an address is not a managed C6.
 */
uint16_t mesh_image_gateway_device_id_from_addr(uint16_t source_addr);

/*
 * Reserve an idle radio window for optional local I/O such as a JPEG HTTP
 * response or one-shot SNTP. While reserved, new OPEN receives BUSY. Always
 * pair a successful try call with mesh_image_gateway_end_idle_work().
 */
bool mesh_image_gateway_try_begin_idle_work(void);
void mesh_image_gateway_end_idle_work(void);

/* Initialize the board-neutral NimBLE BLE Mesh Provisioner and receiver. */
esp_err_t mesh_image_gateway_init(void);

#ifdef __cplusplus
}
#endif

#endif /* MESH_IMAGE_GATEWAY_H */
