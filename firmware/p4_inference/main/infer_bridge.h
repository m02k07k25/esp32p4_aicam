#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int class_id;
    const char *class_name;
    float score;
    float inference_ms;
    uint8_t crop_region;
    uint16_t crop_x;
    uint16_t crop_y;
    uint16_t crop_width;
    uint16_t crop_height;
} infer_result_t;

esp_err_t infer_bridge_init(void);

esp_err_t infer_bridge_process_rgb565(const uint8_t *rgb565_frame,
                                      uint16_t width,
                                      uint16_t height,
                                      infer_result_t *result);

/*
 * Resize the five-crop region selected by infer_bridge_process_rgb565() to the
 * model input dimensions (INFER_INPUT_WIDTH x INFER_INPUT_HEIGHT), preserving
 * RGB565 byte order. The destination must hold width * height * 2 bytes.
 */
esp_err_t infer_bridge_resize_selected_crop_rgb565(const uint8_t *rgb565_frame,
                                                   uint16_t width,
                                                   uint16_t height,
                                                   const infer_result_t *result,
                                                   uint8_t *output_rgb565,
                                                   size_t output_size);

#ifdef __cplusplus
}
#endif
