#include "yolo_bridge.h"

#include <new>
#include <list>
#include <vector>
#include <cstring>

#include "coco_detect.hpp"
#include "dl_image_define.hpp"
#include "dl_image_draw.hpp"
#include "dl_image_jpeg.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "yolo_bridge";
static COCODetect *s_detector = nullptr;
static SemaphoreHandle_t s_infer_lock = nullptr;
static const std::vector<uint8_t> k_box_color_rgb565 = {0x00, 0xF8}; // red in RGB565 little endian

static esp_err_t ensure_output_buffer(uint8_t **jpeg_buf, size_t *capacity, size_t required_size)
{
    if (*jpeg_buf != nullptr && *capacity >= required_size) {
        return ESP_OK;
    }

    uint8_t *new_buf =
        static_cast<uint8_t *>(heap_caps_realloc(*jpeg_buf, required_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (new_buf == nullptr) {
        new_buf = static_cast<uint8_t *>(heap_caps_realloc(*jpeg_buf, required_size, MALLOC_CAP_8BIT));
    }
    if (new_buf == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    *jpeg_buf = new_buf;
    *capacity = required_size;
    return ESP_OK;
}

extern "C" esp_err_t yolo_bridge_init(void)
{
    if (s_infer_lock == nullptr) {
        s_infer_lock = xSemaphoreCreateMutex();
        if (s_infer_lock == nullptr) {
            ESP_LOGE(TAG, "failed to create infer mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_detector == nullptr) {
        // Eager-load model to reduce first HTTP request latency.
        s_detector = new (std::nothrow)
            COCODetect(static_cast<COCODetect::model_type_t>(CONFIG_DEFAULT_COCO_DETECT_MODEL), false);
        if (s_detector == nullptr) {
            ESP_LOGE(TAG, "failed to create COCODetect");
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

extern "C" esp_err_t yolo_bridge_process_rgb565(const uint8_t *rgb565_frame,
                                                 uint16_t width,
                                                 uint16_t height,
                                                 uint8_t jpeg_quality,
                                                 uint8_t **jpeg_buf,
                                                 size_t *jpeg_len,
                                                 size_t *jpeg_buf_capacity,
                                                 int *box_count,
                                                 float *inference_ms)
{
    if (s_detector == nullptr || s_infer_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rgb565_frame == nullptr || jpeg_buf == nullptr || jpeg_len == nullptr || jpeg_buf_capacity == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_infer_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    uint8_t *frame_copy = nullptr;
    int detected_boxes = 0;
    int64_t start_us = 0;
    int64_t end_us = 0;
    std::list<dl::detect::result_t> *results = nullptr;
    dl::image::jpeg_img_t encoded_jpeg = {nullptr, 0};

    size_t frame_size = width * height * 2;
    frame_copy = static_cast<uint8_t *>(heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (frame_copy == nullptr) {
        frame_copy = static_cast<uint8_t *>(heap_caps_malloc(frame_size, MALLOC_CAP_8BIT));
    }
    if (frame_copy == nullptr) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    memcpy(frame_copy, rgb565_frame, frame_size);

    dl::image::img_t frame;
    frame.data = frame_copy;
    frame.width = width;
    frame.height = height;
    frame.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565;

    start_us = esp_timer_get_time();
    results = &s_detector->run(frame);
    end_us = esp_timer_get_time();

    for (const auto &res : *results) {
        if (res.box.size() < 4) {
            continue;
        }
        int x1 = res.box[0];
        int y1 = res.box[1];
        int x2 = res.box[2];
        int y2 = res.box[3];

        if (x1 < 0) {
            x1 = 0;
        }
        if (y1 < 0) {
            y1 = 0;
        }
        if (x2 >= width) {
            x2 = width - 1;
        }
        if (y2 >= height) {
            y2 = height - 1;
        }
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }

        dl::image::draw_hollow_rectangle(frame, x1, y1, x2, y2, k_box_color_rgb565, 3);
        detected_boxes++;
    }

#if CONFIG_SOC_JPEG_CODEC_SUPPORTED
    encoded_jpeg = dl::image::hw_encode_jpeg(frame, 0, jpeg_quality);
#else
    encoded_jpeg = dl::image::sw_encode_jpeg(frame, 0, jpeg_quality);
#endif

    if (encoded_jpeg.data == nullptr || encoded_jpeg.data_len == 0) {
        ESP_LOGE(TAG, "failed to encode detect image");
        ret = ESP_FAIL;
        goto cleanup;
    }

    ret = ensure_output_buffer(jpeg_buf, jpeg_buf_capacity, encoded_jpeg.data_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to grow output jpeg buffer");
        goto cleanup;
    }

    memcpy(*jpeg_buf, encoded_jpeg.data, encoded_jpeg.data_len);
    *jpeg_len = encoded_jpeg.data_len;
    if (box_count != nullptr) {
        *box_count = detected_boxes;
    }
    if (inference_ms != nullptr) {
        *inference_ms = static_cast<float>(end_us - start_us) / 1000.0f;
    }

cleanup:
    if (encoded_jpeg.data != nullptr) {
        heap_caps_free(encoded_jpeg.data);
    }
    if (frame_copy != nullptr) {
        heap_caps_free(frame_copy);
    }
    xSemaphoreGive(s_infer_lock);
    return ret;
}
