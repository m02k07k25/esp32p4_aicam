#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "infer_bridge.h"
#include "sdio_frame_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sdio_frame_tx_init(void);

/*
 * Queue a best-effort SDIO transfer without blocking the caller.  If the C6
 * does not answer, the HTTP handler can still finish while the worker waits
 * for the ESP-Hosted RPC timeout.
 */
esp_err_t sdio_frame_tx_submit_classification(const uint8_t *jpeg,
                                              size_t jpeg_len,
                                              const infer_result_t *result,
                                              float total_ms);

#ifdef __cplusplus
}
#endif
