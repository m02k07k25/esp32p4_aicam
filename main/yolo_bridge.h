#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t yolo_bridge_init(void);

esp_err_t yolo_bridge_process_rgb565(const uint8_t *rgb565_frame,
                                     uint16_t width,
                                     uint16_t height,
                                     uint8_t jpeg_quality,
                                     uint8_t **jpeg_buf,
                                     size_t *jpeg_len,
                                     size_t *jpeg_buf_capacity,
                                     int *box_count,
                                     float *inference_ms);

#ifdef __cplusplus
}
#endif
