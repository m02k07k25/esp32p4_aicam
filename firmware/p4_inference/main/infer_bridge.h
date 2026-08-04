#pragma once

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
} infer_result_t;

esp_err_t infer_bridge_init(void);

esp_err_t infer_bridge_process_rgb565(const uint8_t *rgb565_frame,
                                      uint16_t width,
                                      uint16_t height,
                                      infer_result_t *result);

#ifdef __cplusplus
}
#endif
