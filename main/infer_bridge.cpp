#include "infer_bridge.h"

#include <cstring>
#include <new>
#include <vector>

#include "infer_config.h"
#include "dl_cls_base.hpp"
#include "dl_cls_postprocessor.hpp"
#include "dl_image_define.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern const uint8_t classifier_224_p4_espdl_start[] asm("_binary_classifier_224_p4_espdl_start");

const char *g_infer_labels[INFER_LABEL_COUNT] = {
    "no_human",
    "human",
};

namespace {
static const char *TAG = "infer_bridge";
static const int kInferMutexTimeoutMs = 5000;
static const int kTopK = INFER_LABEL_COUNT;
static const int kCropRegionCount = 5;
static const int kHumanClassIndex = 1;
static const float kScoreThreshold = 0.0f;
static const char *kCropRegionNames[kCropRegionCount] = {
    "top_left",
    "top_right",
    "bottom_left",
    "bottom_right",
    "center",
};
static const std::vector<float> kImageNetMean = {
    123.675f,
    116.280f,
    103.530f,
};
static const std::vector<float> kImageNetStd = {
    58.395f,
    57.120f,
    57.375f,
};

class FixedLabelPostprocessor : public dl::cls::ClsPostprocessor {
public:
    explicit FixedLabelPostprocessor(dl::Model *model) :
        dl::cls::ClsPostprocessor(model, kTopK, kScoreThreshold, true, "")
    {
        m_cat_names = g_infer_labels;
    }
};

class FixedLabelClassifier : public dl::cls::ClsImpl {
public:
    FixedLabelClassifier()
    {
        m_model = new (std::nothrow)
            dl::Model((const char *)classifier_224_p4_espdl_start, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
        if (m_model == nullptr) {
            return;
        }

        if (m_model->get_inputs().size() != 1 || m_model->get_outputs().size() != 1) {
            ESP_LOGE(TAG, "classifier model must expose exactly one input and one output");
            return;
        }

        m_image_preprocessor = new (std::nothrow) dl::image::ImagePreprocessor(m_model, kImageNetMean, kImageNetStd);
        if (m_image_preprocessor == nullptr) {
            ESP_LOGE(TAG, "failed to create image preprocessor");
            return;
        }

        m_postprocessor = new (std::nothrow) FixedLabelPostprocessor(m_model);
        if (m_postprocessor == nullptr) {
            ESP_LOGE(TAG, "failed to create classification postprocessor");
        }
    }

    bool ready() const
    {
        return m_model != nullptr && m_image_preprocessor != nullptr && m_postprocessor != nullptr;
    }

    std::vector<dl::cls::result_t> &run_crop(const dl::image::img_t &img, const std::vector<int> &crop_area)
    {
        m_image_preprocessor->preprocess(img, crop_area);
        m_model->run();
        return m_postprocessor->postprocess();
    }
};

static FixedLabelClassifier *s_classifier = nullptr;
static SemaphoreHandle_t s_infer_lock = nullptr;

static int infer_label_to_index(const char *label)
{
    if (label == nullptr) {
        return -1;
    }

    for (int i = 0; i < INFER_LABEL_COUNT; ++i) {
        if (strcmp(label, g_infer_labels[i]) == 0) {
            return i;
        }
    }

    return -1;
}

static esp_err_t validate_model_shape(FixedLabelClassifier *classifier)
{
    dl::TensorBase *input = classifier->get_raw_model()->get_input();
    if (input == nullptr) {
        ESP_LOGE(TAG, "classifier model input lookup failed");
        return ESP_FAIL;
    }

    if (input->shape.size() != 4 || input->shape[1] != INFER_INPUT_HEIGHT || input->shape[2] != INFER_INPUT_WIDTH ||
        input->shape[3] != 3) {
        ESP_LOGE(TAG,
                 "unexpected classifier input shape, expected [1,%d,%d,3]",
                 INFER_INPUT_HEIGHT,
                 INFER_INPUT_WIDTH);
        return ESP_FAIL;
    }

    return ESP_OK;
}
} // namespace

extern "C" esp_err_t infer_bridge_init(void)
{
    if (s_infer_lock == nullptr) {
        s_infer_lock = xSemaphoreCreateMutex();
        if (s_infer_lock == nullptr) {
            ESP_LOGE(TAG, "failed to create infer mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_classifier == nullptr) {
        s_classifier = new (std::nothrow) FixedLabelClassifier();
        if (s_classifier == nullptr) {
            ESP_LOGE(TAG, "failed to create classifier");
            return ESP_ERR_NO_MEM;
        }

        if (!s_classifier->ready()) {
            ESP_LOGE(TAG, "classifier initialization incomplete");
            delete s_classifier;
            s_classifier = nullptr;
            return ESP_FAIL;
        }

        esp_err_t ret = validate_model_shape(s_classifier);
        if (ret != ESP_OK) {
            delete s_classifier;
            s_classifier = nullptr;
            return ret;
        }

        ESP_LOGI(TAG,
                 "classifier ready: input=%dx%d, labels=%d, preprocess=%s",
                 INFER_INPUT_WIDTH,
                 INFER_INPUT_HEIGHT,
                 INFER_LABEL_COUNT,
                 INFER_PREPROCESS_MODE_NAME);
    }

    return ESP_OK;
}

extern "C" esp_err_t infer_bridge_process_rgb565(const uint8_t *rgb565_frame,
                                                 uint16_t width,
                                                 uint16_t height,
                                                 infer_result_t *result)
{
    if (s_classifier == nullptr || s_infer_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rgb565_frame == nullptr || result == nullptr || width < 2 || height < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_infer_lock, pdMS_TO_TICKS(kInferMutexTimeoutMs)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    dl::image::img_t frame = {
        .data = (void *)rgb565_frame,
        .width = width,
        .height = height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565,
    };

    const int half_width = width / 2;
    const int half_height = height / 2;
    const int center_left = (width - half_width) / 2;
    const int center_top = (height - half_height) / 2;
    const int crop_regions[kCropRegionCount][4] = {
        {0, 0, half_width, half_height},
        {width - half_width, 0, width, half_height},
        {0, height - half_height, half_width, height},
        {width - half_width, height - half_height, width, height},
        {center_left, center_top, center_left + half_width, center_top + half_height},
    };
    std::vector<int> crop_area(4);
    float best_human_score = -1.0f;
    const char *best_class_name = nullptr;
    int best_class_id = -1;
    float best_class_score = 0.0f;
    int best_region_index = -1;

    int64_t start_us = esp_timer_get_time();
    for (int region_index = 0; region_index < kCropRegionCount; ++region_index) {
        for (int coordinate = 0; coordinate < 4; ++coordinate) {
            crop_area[coordinate] = crop_regions[region_index][coordinate];
        }

        std::vector<dl::cls::result_t> &results = s_classifier->run_crop(frame, crop_area);
        if (results.empty()) {
            ESP_LOGE(TAG, "classifier returned no results for region %s", kCropRegionNames[region_index]);
            ret = ESP_FAIL;
            goto cleanup;
        }

        int top_class_id = infer_label_to_index(results.front().cat_name);
        if (top_class_id < 0) {
            ESP_LOGE(TAG, "classifier returned unknown label for region %s", kCropRegionNames[region_index]);
            ret = ESP_FAIL;
            goto cleanup;
        }

        float human_score = -1.0f;
        for (const dl::cls::result_t &region_result : results) {
            if (infer_label_to_index(region_result.cat_name) == kHumanClassIndex) {
                human_score = region_result.score;
                break;
            }
        }
        if (human_score < 0.0f) {
            ESP_LOGE(TAG, "classifier did not return a human score for region %s", kCropRegionNames[region_index]);
            ret = ESP_FAIL;
            goto cleanup;
        }

        ESP_LOGI(TAG,
                 "region=%s crop=[%d,%d,%d,%d] class=%s score=%.4f human_score=%.4f",
                 kCropRegionNames[region_index],
                 crop_area[0],
                 crop_area[1],
                 crop_area[2],
                 crop_area[3],
                 results.front().cat_name,
                 results.front().score,
                 human_score);

        if (human_score > best_human_score) {
            best_human_score = human_score;
            best_class_name = results.front().cat_name;
            best_class_id = top_class_id;
            best_class_score = results.front().score;
            best_region_index = region_index;
        }
    }

    result->class_name = best_class_name;
    result->class_id = best_class_id;
    result->score = best_class_score;
    result->inference_ms = (float)(esp_timer_get_time() - start_us) / 1000.0f;

    if (best_region_index < 0) {
        ESP_LOGE(TAG, "classifier did not select a crop region");
        ret = ESP_FAIL;
    } else {
        ESP_LOGI(TAG,
                 "selected region=%s class=%s score=%.4f human_score=%.4f",
                 kCropRegionNames[best_region_index],
                 result->class_name,
                 result->score,
                 best_human_score);
    }

cleanup:
    xSemaphoreGive(s_infer_lock);
    return ret;
}
