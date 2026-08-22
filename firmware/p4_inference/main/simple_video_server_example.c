/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "sdkconfig.h"
#include "protocol_examples_common.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_task_wdt.h"
#include "driver/jpeg_encode.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "infer_bridge.h"
#include "infer_config.h"
#include "sdio_frame_tx.h"

// video frame buffer count, too large value may cause memory allocation fails.
#define EXAMPLE_VIDEO_BUFFER_COUNT   2
#define MEMORY_TYPE                  V4L2_MEMORY_MMAP
#define CAM_DEV_PATH                 ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define JPEG_ENC_QUALITY             (80)
#define EXAMPLE_MIPI_SCCB_RETRY_DELAY_MS 1000
#define AUTO_INFER_PERIOD_MS          30000
#define AUTO_INFER_TASK_STACK_SIZE    8192
#define AUTO_INFER_TASK_PRIORITY      5
#define AUTO_JPEG_MAX_QUALITY         60
#define AUTO_JPEG_MIN_QUALITY         20
#define AUTO_JPEG_QUALITY_STEP        5
#define AUTO_CROP_BUFFER_ALIGNMENT    64U
#define AUTO_CROP_BUFFER_SIZE         (INFER_INPUT_WIDTH * INFER_INPUT_HEIGHT * 2U)
_Static_assert(INFER_INPUT_WIDTH == 224 && INFER_INPUT_HEIGHT == 224,
               "automatic SDIO human crop must remain exactly 224x224");
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))
#endif
#define EXAMPLE_MDNS_INSTANCE "simple video web"
#define EXAMPLE_MDNS_HOST_NAME "esp-web"

/*
 * Web cam control structure
*/
typedef struct web_cam {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    jpeg_encode_cfg_t jpeg_enc_config;
    size_t jpeg_enc_output_buf_alloced_size;
    jpeg_encoder_handle_t jpeg_handle;
    uint8_t *jpeg_out_buf;
    uint8_t *buffer[EXAMPLE_VIDEO_BUFFER_COUNT];
    SemaphoreHandle_t lock;
} web_cam_t;

/*
 * The image format type definition used in the example.
 */
typedef enum {
    EXAMPLE_VIDEO_FMT_RAW8 = V4L2_PIX_FMT_SBGGR8,
    EXAMPLE_VIDEO_FMT_RAW10 = V4L2_PIX_FMT_SBGGR10,
    EXAMPLE_VIDEO_FMT_GREY = V4L2_PIX_FMT_GREY,
    EXAMPLE_VIDEO_FMT_RGB565 = V4L2_PIX_FMT_RGB565,
    EXAMPLE_VIDEO_FMT_RGB888 = V4L2_PIX_FMT_RGB24,
    EXAMPLE_VIDEO_FMT_YUV422 = V4L2_PIX_FMT_YUV422P,
    EXAMPLE_VIDEO_FMT_YUV420 = V4L2_PIX_FMT_YUV420,
} example_fmt_t;

const int s_queue_buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
static const char *TAG = "example";

static void app_tune_task_wdt(void)
{
#if CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 15000,
        .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U,
        .trigger_panic = false,
    };
    esp_err_t err = esp_task_wdt_reconfigure(&twdt_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Task WDT timeout changed to %u ms", twdt_config.timeout_ms);
    } else {
        ESP_LOGW(TAG, "Task WDT reconfigure failed: %s", esp_err_to_name(err));
    }
#endif
}

#if CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
static const esp_video_init_csi_config_t csi_config[] = {
    {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port      = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_PORT,
                .scl_pin   = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN,
                .sda_pin   = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN,
            },
            .freq = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ,
        },
        .reset_pin = CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_RESET_PIN,
        .pwdn_pin  = CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_PWDN_PIN,
    },
};
#endif

#if CONFIG_EXAMPLE_ENABLE_DVP_CAM_SENSOR
static const esp_video_init_dvp_config_t dvp_config[] = {
    {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port      = CONFIG_EXAMPLE_DVP_SCCB_I2C_PORT,
                .scl_pin   = CONFIG_EXAMPLE_DVP_SCCB_I2C_SCL_PIN,
                .sda_pin   = CONFIG_EXAMPLE_DVP_SCCB_I2C_SDA_PIN,
            },
            .freq      = CONFIG_EXAMPLE_DVP_SCCB_I2C_FREQ,
        },
        .reset_pin = CONFIG_EXAMPLE_DVP_CAM_SENSOR_RESET_PIN,
        .pwdn_pin  = CONFIG_EXAMPLE_DVP_CAM_SENSOR_PWDN_PIN,
        .dvp_pin = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                CONFIG_EXAMPLE_DVP_D0_PIN, CONFIG_EXAMPLE_DVP_D1_PIN, CONFIG_EXAMPLE_DVP_D2_PIN, CONFIG_EXAMPLE_DVP_D3_PIN,
                CONFIG_EXAMPLE_DVP_D4_PIN, CONFIG_EXAMPLE_DVP_D5_PIN, CONFIG_EXAMPLE_DVP_D6_PIN, CONFIG_EXAMPLE_DVP_D7_PIN,
            },
            .vsync_io = CONFIG_EXAMPLE_DVP_VSYNC_PIN,
            .de_io = CONFIG_EXAMPLE_DVP_DE_PIN,
            .pclk_io = CONFIG_EXAMPLE_DVP_PCLK_PIN,
            .xclk_io = CONFIG_EXAMPLE_DVP_XCLK_PIN,
        },
        .xclk_freq = CONFIG_EXAMPLE_DVP_XCLK_FREQ,
    },
};
#endif

static const esp_video_init_config_t cam_config = {
#if CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
    .csi      = csi_config,
#endif
#if CONFIG_EXAMPLE_ENABLE_DVP_CAM_SENSOR
    .dvp      = dvp_config,
#endif
};

static void app_wait_for_video_init(void)
{
    for (int attempt = 1;; attempt++) {
        ESP_LOGI(TAG, "Initializing camera, attempt %d", attempt);
        esp_err_t ret = esp_video_init(&cam_config);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Camera initialized");
            return;
        }

        ESP_LOGW(TAG, "esp_video_init failed: %s; retrying in %d ms",
                 esp_err_to_name(ret), EXAMPLE_MIPI_SCCB_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_MIPI_SCCB_RETRY_DELAY_MS));
    }
}

/**
 * @brief   Open the video device and initialize the video device to use `init_fmt` as the output format.
 * @note    When the sensor outputs data in RAW format, the ISP module can interpolate its data into RGB or YUV format.
 *          However, when the sensor works in RGB or YUV format, the output data can only be in RGB or YUV format.
 * @param dev device name(eg, "/dev/video0")
 * @param init_fmt output format.
 *
 * @return
 *     - Device descriptor   Success
 *     - -1 error
 */
int app_video_open(char *dev, example_fmt_t init_fmt)
{
    struct v4l2_format default_format;
    struct v4l2_capability capability;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "Open video failed");
        return -1;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &capability)) {
        ESP_LOGE(TAG, "failed to get capability");
        goto exit_0;
    }

    ESP_LOGI(TAG, "version: %d.%d.%d", (uint16_t)(capability.version >> 16),
             (uint8_t)(capability.version >> 8),
             (uint8_t)capability.version);
    ESP_LOGI(TAG, "driver:  %s", capability.driver);
    ESP_LOGI(TAG, "card:    %s", capability.card);
    ESP_LOGI(TAG, "bus:     %s", capability.bus_info);

    memset(&default_format, 0, sizeof(struct v4l2_format));
    default_format.type = type;
    if (ioctl(fd, VIDIOC_G_FMT, &default_format) != 0) {
        ESP_LOGE(TAG, "failed to get format");
        goto exit_0;
    }

    ESP_LOGI(TAG, "width=%" PRIu32 " height=%" PRIu32, default_format.fmt.pix.width, default_format.fmt.pix.height);

    if (default_format.fmt.pix.pixelformat != init_fmt) {
        struct v4l2_format format = {
            .type = type,
            .fmt.pix.width = default_format.fmt.pix.width,
            .fmt.pix.height = default_format.fmt.pix.height,
            .fmt.pix.pixelformat = init_fmt,
        };

        if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
            ESP_LOGE(TAG, "failed to set format");
            goto exit_0;
        }
    }

    return fd;
exit_0:
    close(fd);
    return -1;
}

static jpeg_enc_input_format_t get_jpeg_enc_input_fmt(uint32_t video_fmt)
{
    jpeg_enc_input_format_t ret_fmt = JPEG_ENCODE_IN_FORMAT_YUV422;
    switch (video_fmt) {
    case EXAMPLE_VIDEO_FMT_YUV422:
        ret_fmt = JPEG_ENCODE_IN_FORMAT_YUV422;
        break;
    case EXAMPLE_VIDEO_FMT_RAW8: // Treat raw8 as grayscale, for testing only.
    case EXAMPLE_VIDEO_FMT_GREY:
        ret_fmt = JPEG_ENCODE_IN_FORMAT_GRAY;
        break;
    case EXAMPLE_VIDEO_FMT_RGB565:
        ret_fmt = JPEG_ENCODE_IN_FORMAT_RGB565;
        break;
    case EXAMPLE_VIDEO_FMT_RGB888:
        ret_fmt = JPEG_ENCODE_IN_FORMAT_RGB888;
        break;
    default:
        ESP_LOGE(TAG, "Unsupported format");
        ret_fmt = -1;
        break;
    }
    return ret_fmt;
}

/*
 * VIDIOC_STREAMON keeps capturing while the application is idle, but the
 * driver stops advancing once every queued buffer is full. At that point a
 * plain VIDIOC_DQBUF returns the oldest completed frame. Recycle one complete
 * buffer cycle first, then wait for the next frame produced after this request.
 *
 * The caller must hold wc->lock and must requeue fresh_buf when finished.
 */
static esp_err_t dequeue_fresh_video_frame_locked(web_cam_t *wc, struct v4l2_buffer *fresh_buf)
{
    for (size_t i = 0; i < ARRAY_SIZE(wc->buffer); i++) {
        struct v4l2_buffer stale_buf;

        memset(&stale_buf, 0, sizeof(stale_buf));
        stale_buf.type = s_queue_buf_type;
        stale_buf.memory = MEMORY_TYPE;

        if (ioctl(wc->fd, VIDIOC_DQBUF, &stale_buf) != 0) {
            ESP_LOGE(TAG, "failed to dequeue stale video frame");
            return ESP_FAIL;
        }
        if (ioctl(wc->fd, VIDIOC_QBUF, &stale_buf) != 0) {
            ESP_LOGE(TAG, "failed to requeue stale video frame");
            return ESP_FAIL;
        }
    }

    memset(fresh_buf, 0, sizeof(*fresh_buf));
    fresh_buf->type = s_queue_buf_type;
    fresh_buf->memory = MEMORY_TYPE;
    if (ioctl(wc->fd, VIDIOC_DQBUF, fresh_buf) != 0) {
        ESP_LOGE(TAG, "failed to receive fresh video frame");
        return ESP_FAIL;
    }

    if (fresh_buf->index >= ARRAY_SIZE(wc->buffer) ||
        fresh_buf->bytesused == 0 ||
        (fresh_buf->flags & V4L2_BUF_FLAG_ERROR) != 0) {
        ESP_LOGE(TAG,
                 "invalid fresh video frame: index=%u bytes=%u flags=0x%x",
                 (unsigned int)fresh_buf->index,
                 (unsigned int)fresh_buf->bytesused,
                 (unsigned int)fresh_buf->flags);
        if (ioctl(wc->fd, VIDIOC_QBUF, fresh_buf) != 0) {
            ESP_LOGE(TAG, "failed to requeue invalid video frame");
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t record_bin_handler(httpd_req_t *req)
{
    esp_err_t res = ESP_FAIL;
    struct v4l2_buffer buf;
    web_cam_t *wc = (web_cam_t *)req->user_ctx;

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=record.bin"); // default name is record.bin
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (xSemaphoreTake(wc->lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    res = dequeue_fresh_video_frame_locked(wc, &buf);
    if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, (const char *)wc->buffer[buf.index], buf.bytesused);
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "chunk send failed");
        }
    } else {
        ESP_LOGE(TAG, "failed to receive video frame");
        xSemaphoreGive(wc->lock);
        return ESP_FAIL;
    }

    if (ioctl(wc->fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "failed to free video frame");
    }
    xSemaphoreGive(wc->lock);

    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return res;
}

static esp_err_t pic_handler(httpd_req_t *req)
{
    esp_err_t res = ESP_FAIL;
    struct v4l2_buffer buf;
    uint8_t *jpeg_ptr = NULL;
    size_t jpeg_size = 0;
    bool tx_valid = false;
    uint32_t jpeg_encoded_size = 0;
    web_cam_t *wc = (web_cam_t *)req->user_ctx;

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (xSemaphoreTake(wc->lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    res = dequeue_fresh_video_frame_locked(wc, &buf);
    if (res == ESP_OK) {
        if (wc->pixel_format == V4L2_PIX_FMT_JPEG) {
            jpeg_ptr = wc->buffer[buf.index];
            jpeg_size = buf.bytesused;
            tx_valid = true;
        } else {
            res = jpeg_encoder_process(wc->jpeg_handle, &wc->jpeg_enc_config, wc->buffer[buf.index], buf.bytesused, wc->jpeg_out_buf, wc->jpeg_enc_output_buf_alloced_size, &jpeg_encoded_size);
            if (res == ESP_OK) {
                jpeg_ptr = wc->jpeg_out_buf;
                jpeg_size = jpeg_encoded_size;
                tx_valid = true;
                ESP_LOGD(TAG, "jpeg size = %d", jpeg_size);
            } else {
                ESP_LOGE(TAG, "jpeg encode failed");
            }
        }

        if (tx_valid) {
            res = httpd_resp_send_chunk(req, (const char *)jpeg_ptr, jpeg_size);
            if (res != ESP_OK) {
                ESP_LOGE(TAG, "send chunk failed");
            }
        }

        if (ioctl(wc->fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "failed to free video frame");
        }
        xSemaphoreGive(wc->lock);

        /* Respond with an empty chunk to signal HTTP response completion */
        httpd_resp_send_chunk(req, NULL, 0);
    } else {
        ESP_LOGE(TAG, "failed to receive video frame");
        xSemaphoreGive(wc->lock);
    }

    return res;
}

static esp_err_t encode_frame_to_jpeg(web_cam_t *wc,
                                      const uint8_t *frame_buf,
                                      size_t frame_len,
                                      uint8_t jpeg_quality,
                                      uint8_t **jpeg_ptr,
                                      size_t *jpeg_len)
{
    uint32_t jpeg_encoded_size = 0;
    jpeg_encode_cfg_t jpeg_cfg = wc->jpeg_enc_config;

    if (wc->pixel_format == V4L2_PIX_FMT_JPEG) {
        *jpeg_ptr = (uint8_t *)frame_buf;
        *jpeg_len = frame_len;
        return ESP_OK;
    }

    jpeg_cfg.image_quality = jpeg_quality;
    esp_err_t ret = jpeg_encoder_process(wc->jpeg_handle,
                                         &jpeg_cfg,
                                         (uint8_t *)frame_buf,
                                         frame_len,
                                         wc->jpeg_out_buf,
                                         wc->jpeg_enc_output_buf_alloced_size,
                                         &jpeg_encoded_size);
    if (ret != ESP_OK) {
        return ret;
    }

    *jpeg_ptr = wc->jpeg_out_buf;
    *jpeg_len = jpeg_encoded_size;
    return ESP_OK;
}

static esp_err_t encode_rgb565_to_jpeg(web_cam_t *wc,
                                       uint8_t *rgb565,
                                       uint32_t width,
                                       uint32_t height,
                                       uint8_t jpeg_quality,
                                       uint8_t **jpeg_ptr,
                                       size_t *jpeg_len)
{
    jpeg_encode_cfg_t jpeg_cfg = wc->jpeg_enc_config;
    uint32_t jpeg_encoded_size = 0;

    jpeg_cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    jpeg_cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
    jpeg_cfg.width = width;
    jpeg_cfg.height = height;
    jpeg_cfg.image_quality = jpeg_quality;

    esp_err_t ret = jpeg_encoder_process(wc->jpeg_handle,
                                         &jpeg_cfg,
                                         rgb565,
                                         width * height * 2U,
                                         wc->jpeg_out_buf,
                                         wc->jpeg_enc_output_buf_alloced_size,
                                         &jpeg_encoded_size);
    if (ret != ESP_OK) {
        return ret;
    }

    *jpeg_ptr = wc->jpeg_out_buf;
    *jpeg_len = jpeg_encoded_size;
    return ESP_OK;
}

static esp_err_t encode_bounded_classification_crop_jpeg(web_cam_t *wc,
                                                          uint8_t *crop_rgb565,
                                                          uint8_t **jpeg_ptr,
                                                          size_t *jpeg_len,
                                                          uint8_t *selected_quality)
{
    for (int quality = AUTO_JPEG_MAX_QUALITY;
         quality >= AUTO_JPEG_MIN_QUALITY;
         quality -= AUTO_JPEG_QUALITY_STEP) {
        esp_err_t ret = encode_rgb565_to_jpeg(wc,
                                              crop_rgb565,
                                              INFER_INPUT_WIDTH,
                                              INFER_INPUT_HEIGHT,
                                              (uint8_t)quality,
                                              jpeg_ptr,
                                              jpeg_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "JPEG encode %dx%d q=%d failed: %s",
                     INFER_INPUT_WIDTH,
                     INFER_INPUT_HEIGHT,
                     quality,
                     esp_err_to_name(ret));
            continue;
        }

        ESP_LOGD(TAG,
                 "JPEG crop candidate %dx%d q=%d size=%u B",
                 INFER_INPUT_WIDTH,
                 INFER_INPUT_HEIGHT,
                 quality,
                 (unsigned int)*jpeg_len);
        if (*jpeg_len <= SDIO_FRAME_MAX_JPEG_SIZE) {
            *selected_quality = (uint8_t)quality;
            return ESP_OK;
        }
    }

    return ESP_ERR_INVALID_SIZE;
}

static esp_err_t classify_handler(httpd_req_t *req)
{
    esp_err_t res = ESP_FAIL;
    struct v4l2_buffer buf;
    uint8_t *jpeg_ptr = NULL;
    size_t jpeg_len = 0;
    infer_result_t infer_result = {0};
    float total_ms = 0;
    web_cam_t *wc = (web_cam_t *)req->user_ctx;
    int64_t req_start_us = esp_timer_get_time();

    if (wc->pixel_format != EXAMPLE_VIDEO_FMT_RGB565) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Classifier handler requires RGB565 camera format");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=classify.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (xSemaphoreTake(wc->lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    res = dequeue_fresh_video_frame_locked(wc, &buf);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "failed to receive video frame");
        xSemaphoreGive(wc->lock);
        return ESP_FAIL;
    }

    res = infer_bridge_process_rgb565(wc->buffer[buf.index], wc->width, wc->height, &infer_result);
    if (res == ESP_OK) {
        res = encode_frame_to_jpeg(
            wc, wc->buffer[buf.index], buf.bytesused, INFER_CLASSIFY_JPEG_QUALITY, &jpeg_ptr, &jpeg_len);
    }

    if (ioctl(wc->fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "failed to free video frame");
        if (res == ESP_OK) {
            res = ESP_FAIL;
        }
    }

    if (res != ESP_OK) {
        ESP_LOGE(TAG, "classification failed (%s)", esp_err_to_name(res));
        xSemaphoreGive(wc->lock);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "classification failed");
        return res;
    }

    /* httpd_resp_set_hdr() keeps header value pointers until send time. */
    char class_id_hdr[32];
    char score_hdr[32];
    char inference_ms_hdr[32];
    char total_ms_hdr[32];
    snprintf(class_id_hdr, sizeof(class_id_hdr), "%d", infer_result.class_id);
    httpd_resp_set_hdr(req, "X-Class-Index", class_id_hdr);
    httpd_resp_set_hdr(req, "X-Class-Label", infer_result.class_name);
    snprintf(score_hdr, sizeof(score_hdr), "%.4f", infer_result.score);
    httpd_resp_set_hdr(req, "X-Class-Score", score_hdr);
    snprintf(inference_ms_hdr, sizeof(inference_ms_hdr), "%.2f", infer_result.inference_ms);
    httpd_resp_set_hdr(req, "X-Inference-Time-Ms", inference_ms_hdr);
    total_ms = (float)(esp_timer_get_time() - req_start_us) / 1000.0f;
    snprintf(total_ms_hdr, sizeof(total_ms_hdr), "%.2f", total_ms);
    httpd_resp_set_hdr(req, "X-Inference-Total-Ms", total_ms_hdr);
    ESP_LOGI(TAG,
             "classification done: class=%d (%s), score=%.4f, inference=%.2f ms, total=%.2f ms, jpeg=%u B",
             infer_result.class_id,
             infer_result.class_name,
             infer_result.score,
             infer_result.inference_ms,
             total_ms,
             (unsigned int)jpeg_len);

    res = httpd_resp_send_chunk(req, (const char *)jpeg_ptr, jpeg_len);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "send classify chunk failed");
        xSemaphoreGive(wc->lock);
        return res;
    }

    res = httpd_resp_send_chunk(req, NULL, 0);
    xSemaphoreGive(wc->lock);
    return res;
}

static void run_periodic_inference(web_cam_t *wc, uint8_t *crop_rgb565)
{
    struct v4l2_buffer buf;
    infer_result_t infer_result = {0};
    uint64_t detected_at_ms = 0;
    const int64_t start_us = esp_timer_get_time();

    if (xSemaphoreTake(wc->lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGW(TAG, "30-second inference skipped: camera lock timeout");
        return;
    }

    esp_err_t ret = dequeue_fresh_video_frame_locked(wc, &buf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "30-second inference failed to capture a fresh frame");
        xSemaphoreGive(wc->lock);
        return;
    }
    /* This fresh buffer was produced after the dequeue request. Timestamp it
     * before inference using the server-authoritative clock exchanged through
     * C6. Mesh congestion and JPEG encoding therefore cannot shift capture
     * time. Zero explicitly means no fresh server time mapping is available. */
    detected_at_ms = sdio_frame_tx_capture_time_ms();

    const size_t required_rgb565_size = (size_t)wc->width * wc->height * 2U;
    if (wc->pixel_format != EXAMPLE_VIDEO_FMT_RGB565 ||
        buf.bytesused < required_rgb565_size) {
        ESP_LOGW(TAG,
                 "30-second inference got invalid RGB565 frame: format=%08" PRIx32
                 " bytes=%u expected=%u",
                 wc->pixel_format,
                 (unsigned int)buf.bytesused,
                 (unsigned int)required_rgb565_size);
        ret = ESP_ERR_INVALID_SIZE;
        goto requeue_frame;
    }

    ret = infer_bridge_process_rgb565(
        wc->buffer[buf.index], wc->width, wc->height, &infer_result);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "30-second inference failed: %s", esp_err_to_name(ret));
        goto requeue_frame;
    }

    ESP_LOGI(TAG,
             "30-second inference: class=%d (%s) score=%.4f inference=%.2f ms",
             infer_result.class_id,
             infer_result.class_name,
             infer_result.score,
             infer_result.inference_ms);

    if (infer_result.class_id != INFER_HUMAN_CLASS_ID) {
        goto requeue_frame;
    }
    if (!sdio_frame_tx_remote_ready()) {
        ESP_LOGI(TAG, "human detected, but C6 is not READY; crop/JPEG work skipped");
        goto requeue_frame;
    }
    if (crop_rgb565 == NULL) {
        ESP_LOGW(TAG, "human detected and C6 READY, but the 224x224 crop buffer is unavailable");
        goto requeue_frame;
    }

    ret = infer_bridge_resize_selected_crop_rgb565(wc->buffer[buf.index],
                                                    wc->width,
                                                    wc->height,
                                                    &infer_result,
                                                    crop_rgb565,
                                                    AUTO_CROP_BUFFER_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "failed to export selected human crop region=%u x=%u y=%u w=%u h=%u: %s",
                 infer_result.crop_region,
                 infer_result.crop_x,
                 infer_result.crop_y,
                 infer_result.crop_width,
                 infer_result.crop_height,
                 esp_err_to_name(ret));
        goto requeue_frame;
    }

    uint8_t *jpeg_ptr = NULL;
    size_t jpeg_len = 0;
    uint8_t selected_quality = 0;
    ret = encode_bounded_classification_crop_jpeg(wc,
                                                  crop_rgb565,
                                                  &jpeg_ptr,
                                                  &jpeg_len,
                                                  &selected_quality);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "no 224x224 crop JPEG fit within %u B after q=%d..%d ladder",
                 SDIO_FRAME_MAX_JPEG_SIZE,
                 AUTO_JPEG_MAX_QUALITY,
                 AUTO_JPEG_MIN_QUALITY);
        goto requeue_frame;
    }

    const float total_ms = (float)(esp_timer_get_time() - start_us) / 1000.0f;
    ret = sdio_frame_tx_submit_event(jpeg_ptr, jpeg_len, detected_at_ms);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "queued human crop region=%u src=[%u,%u,%u,%u] JPEG=%dx%d"
                 " q=%u size=%u B detected_at_ms=%" PRIu64 " total=%.2f ms",
                 infer_result.crop_region,
                 infer_result.crop_x,
                 infer_result.crop_y,
                 infer_result.crop_width,
                 infer_result.crop_height,
                 INFER_INPUT_WIDTH,
                 INFER_INPUT_HEIGHT,
                 selected_quality,
                 (unsigned int)jpeg_len,
                 detected_at_ms,
                 total_ms);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "C6 READY changed while encoding; automatic JPEG was not queued");
    } else {
        ESP_LOGW(TAG, "automatic SDIO frame queue failed: %s", esp_err_to_name(ret));
    }

requeue_frame:
    if (ioctl(wc->fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "failed to requeue automatic inference frame");
    }
    xSemaphoreGive(wc->lock);
}

static void periodic_inference_task(void *arg)
{
    web_cam_t *wc = (web_cam_t *)arg;
    uint8_t *crop_rgb565 = NULL;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(AUTO_INFER_PERIOD_MS));

        if (crop_rgb565 == NULL) {
            crop_rgb565 = heap_caps_aligned_calloc(AUTO_CROP_BUFFER_ALIGNMENT,
                                                   1,
                                                   AUTO_CROP_BUFFER_SIZE,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            if (crop_rgb565 == NULL) {
                ESP_LOGW(TAG, "failed to allocate %u-byte 224x224 RGB565 crop buffer; will retry",
                         AUTO_CROP_BUFFER_SIZE);
            }
        }

        run_periodic_inference(wc, crop_rgb565);
    }
}

static esp_err_t start_periodic_inference(web_cam_t *wc)
{
    if (wc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xTaskCreate(periodic_inference_task,
                    "periodic_infer",
                    AUTO_INFER_TASK_STACK_SIZE,
                    wc,
                    AUTO_INFER_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t new_web_cam(int cam_fd, web_cam_t **ret_wc)
{
    int ret;
    struct v4l2_format format;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_requestbuffers req;
    web_cam_t *wc;
    size_t jpeg_enc_input_src_size;

    memset(&format, 0, sizeof(struct v4l2_format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "Failed get fmt");
        return ESP_FAIL;
    }

    wc = malloc(sizeof(web_cam_t));
    if (!wc) {
        return ESP_ERR_NO_MEM;
    }
    memset(wc, 0, sizeof(*wc));

    wc->fd = cam_fd;
    wc->width = format.fmt.pix.width;
    wc->height = format.fmt.pix.height;
    wc->pixel_format = format.fmt.pix.pixelformat;
    wc->lock = xSemaphoreCreateMutex();
    if (wc->lock == NULL) {
        free(wc);
        return ESP_ERR_NO_MEM;
    }

    jpeg_enc_input_format_t jpeg_enc_infmt = get_jpeg_enc_input_fmt(format.fmt.pix.pixelformat);

    wc->jpeg_enc_config.src_type = jpeg_enc_infmt;
    wc->jpeg_enc_config.image_quality = JPEG_ENC_QUALITY;
    wc->jpeg_enc_config.width = format.fmt.pix.width;
    wc->jpeg_enc_config.height = format.fmt.pix.height;

    if (wc->pixel_format == EXAMPLE_VIDEO_FMT_RAW8) {
        wc->jpeg_enc_config.sub_sample = JPEG_DOWN_SAMPLING_GRAY;
        jpeg_enc_input_src_size = format.fmt.pix.width * format.fmt.pix.height;
    } else if (wc->pixel_format == EXAMPLE_VIDEO_FMT_GREY) {
        wc->jpeg_enc_config.sub_sample = JPEG_DOWN_SAMPLING_GRAY;
        jpeg_enc_input_src_size = format.fmt.pix.width * format.fmt.pix.height;
    } else if (wc->pixel_format == EXAMPLE_VIDEO_FMT_YUV420) {
        wc->jpeg_enc_config.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
        jpeg_enc_input_src_size = format.fmt.pix.width * format.fmt.pix.height * 3 / 2;
    } else {
        wc->jpeg_enc_config.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
        jpeg_enc_input_src_size = format.fmt.pix.width * format.fmt.pix.height * 2;
    }

    jpeg_encode_engine_cfg_t encode_eng_cfg = {
        .timeout_ms = 5000,
    };
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&encode_eng_cfg, &wc->jpeg_handle));

    jpeg_encode_memory_alloc_cfg_t jpeg_enc_output_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    // Note that a larger JPEG_ENC_QUALITY means better image quality, so you need to increase the allocated buffer size
    wc->jpeg_out_buf = (uint8_t *)jpeg_alloc_encoder_mem(jpeg_enc_input_src_size / 2, &jpeg_enc_output_mem_cfg, &wc->jpeg_enc_output_buf_alloced_size);
    if (!wc->jpeg_out_buf) {
        ESP_LOGE(TAG, "failed to alloc jpeg output buf");
        ret = ESP_ERR_NO_MEM;
        goto errout;
    }

    memset(&req, 0, sizeof(req));
    req.count  = ARRAY_SIZE(wc->buffer);
    req.type   = type;
    req.memory = MEMORY_TYPE;
    if (ioctl(wc->fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "failed to req buffers");
        ret = ESP_FAIL;
        goto errout;
    }

    for (int i = 0; i < ARRAY_SIZE(wc->buffer); i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type        = type;
        buf.memory      = MEMORY_TYPE;
        buf.index       = i;
        if (ioctl(wc->fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "failed to query buffer");
            ret = ESP_FAIL;
            goto errout;
        }

        wc->buffer[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, wc->fd, buf.m.offset);
        if (!wc->buffer[i]) {
            ESP_LOGE(TAG, "failed to map buffer");
            ret = ESP_FAIL;
            goto errout;
        }

        if (ioctl(wc->fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "failed to queue frame buffer");
            ret = ESP_FAIL;
            goto errout;
        }
    }

    if (ioctl(wc->fd, VIDIOC_STREAMON, &type)) {
        ESP_LOGE(TAG, "failed to start stream");
        ret = ESP_FAIL;
        goto errout;
    }

    *ret_wc = wc;
    return ESP_OK;

errout:
    if (wc->lock != NULL) {
        vSemaphoreDelete(wc->lock);
    }
    free(wc);
    return ret;
}

static esp_err_t http_server_init(int index, web_cam_t *web_cam)
{
    esp_err_t ret;
    httpd_handle_t video_web_httpd;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 1024 * 8;
    config.server_port += index;
    config.ctrl_port += index;

    httpd_uri_t pic_get_uri = {
        .uri = "/pic",
        .method = HTTP_GET,
        .handler = pic_handler,
        .user_ctx = (void *)web_cam
    };
    httpd_uri_t record_file_get_uri = {
        .uri = "/record",
        .method = HTTP_GET,
        .handler = record_bin_handler,
        .user_ctx = (void *)web_cam,
    };
    httpd_uri_t classify_get_uri = {
        .uri = "/classify.jpg",
        .method = HTTP_GET,
        .handler = classify_handler,
        .user_ctx = (void *)web_cam
    };

    ret = httpd_start(&video_web_httpd, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ret;
    }

    ESP_ERROR_CHECK(httpd_register_uri_handler(video_web_httpd, &pic_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(video_web_httpd, &record_file_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(video_web_httpd, &classify_get_uri));

    ESP_LOGI(TAG, "Starting HTTP server on port: '%d' (/pic, /record, /classify.jpg)", config.server_port);

    return ESP_OK;
}

/**
 * @brief   Build a web server with `cam_fd` as the image data source.
 * @param index The index number of the web server.
 * It is allowed to establish multiple servers, and its data port and control port are the default port + index
 * @param cam_fd Cam device descriptor.
 *
 * @return
 *     - ESP_OK   Success
 *     - Others error
 */
static esp_err_t start_cam_web_server(int index, int cam_fd, web_cam_t **ret_web_cam)
{
    web_cam_t *web_cam;
    esp_err_t ret = new_web_cam(cam_fd, &web_cam);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to new web cam");
        return ret;
    }
    ret = http_server_init(index, web_cam);
    if (ret == ESP_OK) {
        *ret_web_cam = web_cam;
    }
    return ret;
}

static void initialise_mdns(void)
{
    mdns_init();
    mdns_hostname_set(EXAMPLE_MDNS_HOST_NAME);
    mdns_instance_name_set(EXAMPLE_MDNS_INSTANCE);

    mdns_txt_item_t serviceTxtData[] = {
        {"board", CONFIG_IDF_TARGET},
        {"path", "/"}
    };

    ESP_ERROR_CHECK(mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, serviceTxtData,
                                     sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
}

void app_main(void)
{
    int index = 0;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    app_tune_task_wdt();

    /* Initialize the camera before network setup so sensors that depend on
     * early clock/power sequencing are detected right after boot.
     */
    app_wait_for_video_init();

    /* ESP-Hosted transport events use the default event loop even when the
     * optional HTTP/network path is disabled.  Creating this loop does not
     * start Wi-Fi/Ethernet or wait for an IP address. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_P4_ENABLE_HTTP
    ESP_ERROR_CHECK(esp_netif_init());

    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(EXAMPLE_MDNS_HOST_NAME);

    /* The diagnostic HTTP server is the only P4 feature that needs a
     * network interface.  The autonomous inference/SDIO path intentionally
     * does not wait for Ethernet or Wi-Fi. */
    ESP_ERROR_CHECK(example_connect());
#else
    ESP_LOGI(TAG, "P4 HTTP disabled; skipping Ethernet/Wi-Fi connection");
#endif

    int video_cam_fd = app_video_open(CAM_DEV_PATH, EXAMPLE_VIDEO_FMT_RGB565);
    if (video_cam_fd < 0) {
        ESP_LOGE(TAG, "video cam open failed");
        return;
    }

    ESP_ERROR_CHECK(infer_bridge_init());
    esp_err_t sdio_ret = sdio_frame_tx_init();
    if (sdio_ret != ESP_OK && sdio_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Failed to start optional SDIO frame sender: %s", esp_err_to_name(sdio_ret));
    }
    web_cam_t *web_cam = NULL;
#if CONFIG_P4_ENABLE_HTTP
    ESP_ERROR_CHECK(start_cam_web_server(index, video_cam_fd, &web_cam));
#else
    ESP_ERROR_CHECK(new_web_cam(video_cam_fd, &web_cam));
#endif
    ESP_ERROR_CHECK(start_periodic_inference(web_cam));
    ESP_LOGI(TAG, "automatic inference scheduled every %u ms", AUTO_INFER_PERIOD_MS);
    ESP_LOGI(TAG, "Example Start");
}
