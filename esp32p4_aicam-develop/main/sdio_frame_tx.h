#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "infer_bridge.h"
#include "sdio_frame_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sdio_frame_tx_send_classification(const uint8_t *jpeg,
                                            size_t jpeg_len,
                                            const infer_result_t *result,
                                            float total_ms);

#ifdef __cplusplus
}
#endif
