/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#if CONFIG_EXAMPLE_CAPTURE_SD_PWR_CTRL_LDO_INTERNAL_IO
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
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
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))
#endif
#define EXAMPLE_MDNS_INSTANCE "simple video web"
#define EXAMPLE_MDNS_HOST_NAME "esp-web"
#define DATA_AP_SSID "esp32p4-data"
#define DATA_AP_PASSWORD "12345678"
#define DATA_AP_CHANNEL 1
#define DATA_AP_MAX_CONN 4
#define CAPTURE_MOUNT_POINT "/sdcard"
#define CAPTURE_DIR CAPTURE_MOUNT_POINT "/captures"
#define CAPTURE_NAME_PREFIX "IMG"
#define CAPTURE_NAME_DIGITS 5
#define CAPTURE_FILE_EXT ".JPG"
#define CAPTURE_FILENAME_MAX_LEN 16
#define CAPTURE_PATH_MAX_LEN 64
#define CAPTURE_TRIGGER_GPIO GPIO_NUM_1
#define CAPTURE_TRIGGER_DEBOUNCE_MS 300
#define CAPTURE_TRIGGER_TASK_STACK_SIZE 6144
#define CAPTURE_TRIGGER_TASK_PRIORITY 5

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
static sdmmc_card_t *s_sd_card;
static bool s_capture_storage_mounted;
static uint32_t s_next_capture_index = 1;
static SemaphoreHandle_t s_capture_index_lock;
static TaskHandle_t s_capture_button_task_handle;
#if CONFIG_EXAMPLE_CAPTURE_SD_PWR_CTRL_LDO_INTERNAL_IO
static sd_pwr_ctrl_handle_t s_sd_pwr_ctrl_handle;
#endif

typedef esp_err_t (*jpeg_frame_consumer_t)(const uint8_t *jpeg, size_t jpeg_len, void *ctx);

static esp_err_t app_capture_save_photo(web_cam_t *wc, char *saved_name, size_t saved_name_size);

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

static bool is_capture_filename(const char *name)
{
    if (name == NULL) {
        return false;
    }

    size_t name_len = strlen(name);
    size_t expected_len = strlen(CAPTURE_NAME_PREFIX) + CAPTURE_NAME_DIGITS + strlen(CAPTURE_FILE_EXT);
    if (name_len != expected_len) {
        return false;
    }

    if (strncmp(name, CAPTURE_NAME_PREFIX, strlen(CAPTURE_NAME_PREFIX)) != 0) {
        return false;
    }

    const char *digits = name + strlen(CAPTURE_NAME_PREFIX);
    for (int i = 0; i < CAPTURE_NAME_DIGITS; i++) {
        if (digits[i] < '0' || digits[i] > '9') {
            return false;
        }
    }

    return strcmp(digits + CAPTURE_NAME_DIGITS, CAPTURE_FILE_EXT) == 0;
}

static esp_err_t make_capture_filename(uint32_t index, char *name, size_t name_size)
{
    int written = snprintf(name,
                           name_size,
                           CAPTURE_NAME_PREFIX "%0*" PRIu32 CAPTURE_FILE_EXT,
                           CAPTURE_NAME_DIGITS,
                           index);
    if (written < 0 || written >= name_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t make_capture_path(const char *name, char *path, size_t path_size)
{
    if (!is_capture_filename(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(path, path_size, CAPTURE_DIR "/%s", name);
    if (written < 0 || written >= path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static void init_next_capture_index(void)
{
    uint32_t max_index = 0;
    DIR *dir = opendir(CAPTURE_DIR);
    if (dir == NULL) {
        s_next_capture_index = 1;
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_capture_filename(entry->d_name)) {
            continue;
        }

        uint32_t index = 0;
        if (sscanf(entry->d_name, CAPTURE_NAME_PREFIX "%" SCNu32 CAPTURE_FILE_EXT, &index) == 1) {
            max_index = MAX(max_index, index);
        }
    }

    closedir(dir);
    s_next_capture_index = max_index + 1;
}

static esp_err_t app_capture_storage_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_err_t ret;

    host.slot = CONFIG_EXAMPLE_CAPTURE_SDMMC_SLOT;
#if CONFIG_EXAMPLE_CAPTURE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_EXAMPLE_CAPTURE_SD_PWR_CTRL_LDO_IO_ID,
    };

    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_sd_pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD power LDO init failed: %s", esp_err_to_name(ret));
        s_capture_storage_mounted = false;
        return ret;
    }
    host.pwr_ctrl_handle = s_sd_pwr_ctrl_handle;
#endif
#if CONFIG_EXAMPLE_CAPTURE_SDMMC_BUS_WIDTH_4
    slot_config.width = 4;
#else
    slot_config.width = 1;
#endif
    slot_config.clk = CONFIG_EXAMPLE_CAPTURE_SDMMC_CLK_PIN;
    slot_config.cmd = CONFIG_EXAMPLE_CAPTURE_SDMMC_CMD_PIN;
    slot_config.d0 = CONFIG_EXAMPLE_CAPTURE_SDMMC_D0_PIN;
#if CONFIG_EXAMPLE_CAPTURE_SDMMC_BUS_WIDTH_4
    slot_config.d1 = CONFIG_EXAMPLE_CAPTURE_SDMMC_D1_PIN;
    slot_config.d2 = CONFIG_EXAMPLE_CAPTURE_SDMMC_D2_PIN;
    slot_config.d3 = CONFIG_EXAMPLE_CAPTURE_SDMMC_D3_PIN;
#endif
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting SD card at %s using SDMMC slot %d width %d GPIOs: CLK[%d] CMD[%d] D0[%d]",
             CAPTURE_MOUNT_POINT,
             host.slot,
             slot_config.width,
             slot_config.clk,
             slot_config.cmd,
             slot_config.d0);
    ret = esp_vfs_fat_sdmmc_mount(CAPTURE_MOUNT_POINT, &host, &slot_config, &mount_config, &s_sd_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
#if CONFIG_EXAMPLE_CAPTURE_SD_PWR_CTRL_LDO_INTERNAL_IO
        if (s_sd_pwr_ctrl_handle != NULL) {
            esp_err_t pwr_ret = sd_pwr_ctrl_del_on_chip_ldo(s_sd_pwr_ctrl_handle);
            if (pwr_ret != ESP_OK) {
                ESP_LOGW(TAG, "SD power LDO cleanup failed: %s", esp_err_to_name(pwr_ret));
            }
            s_sd_pwr_ctrl_handle = NULL;
        }
#endif
        s_capture_storage_mounted = false;
        return ret;
    }

    if (mkdir(CAPTURE_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "Failed to create capture dir %s: errno=%d", CAPTURE_DIR, errno);
        esp_vfs_fat_sdcard_unmount(CAPTURE_MOUNT_POINT, s_sd_card);
#if CONFIG_EXAMPLE_CAPTURE_SD_PWR_CTRL_LDO_INTERNAL_IO
        if (s_sd_pwr_ctrl_handle != NULL) {
            esp_err_t pwr_ret = sd_pwr_ctrl_del_on_chip_ldo(s_sd_pwr_ctrl_handle);
            if (pwr_ret != ESP_OK) {
                ESP_LOGW(TAG, "SD power LDO cleanup failed: %s", esp_err_to_name(pwr_ret));
            }
            s_sd_pwr_ctrl_handle = NULL;
        }
#endif
        s_sd_card = NULL;
        s_capture_storage_mounted = false;
        return ESP_FAIL;
    }

    init_next_capture_index();
    s_capture_storage_mounted = true;
    ESP_LOGI(TAG, "SD capture storage ready, next image index=%" PRIu32, s_next_capture_index);
    sdmmc_card_print_info(stdout, s_sd_card);
    return ESP_OK;
}

static esp_err_t app_wifi_softap_start(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = DATA_AP_SSID,
            .ssid_len = strlen(DATA_AP_SSID),
            .channel = DATA_AP_CHANNEL,
            .password = DATA_AP_PASSWORD,
            .max_connection = DATA_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    if (strlen(DATA_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: ssid=%s password=%s url=http://192.168.4.1/captures",
             DATA_AP_SSID,
             DATA_AP_PASSWORD);
    return ESP_OK;
}

static void IRAM_ATTR app_capture_button_isr_handler(void *arg)
{
    (void)arg;

    BaseType_t high_task_woken = pdFALSE;
    if (s_capture_button_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_capture_button_task_handle, &high_task_woken);
        if (high_task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

static void app_capture_button_task(void *arg)
{
    web_cam_t *web_cam = (web_cam_t *)arg;
    int64_t last_capture_us = 0;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_capture_us < CAPTURE_TRIGGER_DEBOUNCE_MS * 1000LL) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level(CAPTURE_TRIGGER_GPIO) != 0) {
            continue;
        }

        last_capture_us = esp_timer_get_time();
        esp_err_t ret = app_capture_save_photo(web_cam, NULL, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "GPIO capture failed: %s", esp_err_to_name(ret));
        }

        while (gpio_get_level(CAPTURE_TRIGGER_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

static void app_capture_button_init(web_cam_t *web_cam)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CAPTURE_TRIGGER_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "capture trigger GPIO config failed: %s", esp_err_to_name(ret));
        return;
    }

    if (s_capture_button_task_handle == NULL) {
        BaseType_t task_ret = xTaskCreate(app_capture_button_task,
                                          "capture_button",
                                          CAPTURE_TRIGGER_TASK_STACK_SIZE,
                                          web_cam,
                                          CAPTURE_TRIGGER_TASK_PRIORITY,
                                          &s_capture_button_task_handle);
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "failed to create capture button task");
            return;
        }
    }

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "GPIO ISR service install failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = gpio_isr_handler_add(CAPTURE_TRIGGER_GPIO, app_capture_button_isr_handler, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "capture trigger ISR add failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Capture trigger ready: GPIO%d with internal pull-up, active-low to GND",
             CAPTURE_TRIGGER_GPIO);
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

static esp_err_t record_bin_handler(httpd_req_t *req)
{
    esp_err_t res = ESP_FAIL;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_buffer buf;
    web_cam_t *wc = (web_cam_t *)req->user_ctx;

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=record.bin"); // default name is record.bin
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (xSemaphoreTake(wc->lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type   = type;
    buf.memory = MEMORY_TYPE;
    res = ioctl(wc->fd, VIDIOC_DQBUF, &buf);
    if (res == 0) {
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

    memset(&buf, 0, sizeof(buf));
    buf.type   = s_queue_buf_type;
    buf.memory = MEMORY_TYPE;
    res = ioctl(wc->fd, VIDIOC_DQBUF, &buf);
    if (res == 0) {
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

static esp_err_t capture_jpeg_frame(web_cam_t *wc,
                                    uint8_t jpeg_quality,
                                    jpeg_frame_consumer_t consumer,
                                    void *consumer_ctx)
{
    esp_err_t ret = ESP_FAIL;
    struct v4l2_buffer buf;
    uint8_t *jpeg_ptr = NULL;
    size_t jpeg_len = 0;

    if (xSemaphoreTake(wc->lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = s_queue_buf_type;
    buf.memory = MEMORY_TYPE;
    if (ioctl(wc->fd, VIDIOC_DQBUF, &buf) != 0) {
        ESP_LOGE(TAG, "failed to receive video frame");
        xSemaphoreGive(wc->lock);
        return ESP_FAIL;
    }

    ret = encode_frame_to_jpeg(wc, wc->buffer[buf.index], buf.bytesused, jpeg_quality, &jpeg_ptr, &jpeg_len);
    if (ret == ESP_OK) {
        ret = consumer(jpeg_ptr, jpeg_len, consumer_ctx);
    } else {
        ESP_LOGE(TAG, "jpeg encode failed: %s", esp_err_to_name(ret));
    }

    if (ioctl(wc->fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "failed to free video frame");
        if (ret == ESP_OK) {
            ret = ESP_FAIL;
        }
    }

    xSemaphoreGive(wc->lock);
    return ret;
}

typedef struct {
    const char *path;
} file_write_ctx_t;

static esp_err_t write_jpeg_file_consumer(const uint8_t *jpeg, size_t jpeg_len, void *ctx)
{
    file_write_ctx_t *write_ctx = (file_write_ctx_t *)ctx;
    FILE *file = fopen(write_ctx->path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for write: errno=%d", write_ctx->path, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(jpeg, 1, jpeg_len, file);
    int close_ret = fclose(file);
    if (written != jpeg_len || close_ret != 0) {
        ESP_LOGE(TAG, "Failed to write %s: written=%u/%u close=%d errno=%d",
                 write_ctx->path,
                 (unsigned int)written,
                 (unsigned int)jpeg_len,
                 close_ret,
                 errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t app_capture_save_photo(web_cam_t *wc, char *saved_name, size_t saved_name_size)
{
    if (!s_capture_storage_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char name[CAPTURE_FILENAME_MAX_LEN];
    char path[CAPTURE_PATH_MAX_LEN];

    if (xSemaphoreTake(s_capture_index_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t index = s_next_capture_index++;
    xSemaphoreGive(s_capture_index_lock);

    esp_err_t ret = make_capture_filename(index, name, sizeof(name));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = make_capture_path(name, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }

    file_write_ctx_t write_ctx = {
        .path = path,
    };
    ret = capture_jpeg_frame(wc, JPEG_ENC_QUALITY, write_jpeg_file_consumer, &write_ctx);
    if (ret != ESP_OK) {
        unlink(path);
        return ret;
    }

    if (saved_name != NULL && saved_name_size > 0) {
        snprintf(saved_name, saved_name_size, "%s", name);
    }

    ESP_LOGI(TAG, "Saved capture: %s", path);
    return ESP_OK;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    char saved_name[CAPTURE_FILENAME_MAX_LEN] = {0};
    web_cam_t *wc = (web_cam_t *)req->user_ctx;

    esp_err_t ret = app_capture_save_photo(wc, saved_name, sizeof(saved_name));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "capture save failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req,
                            HTTPD_500_INTERNAL_SERVER_ERROR,
                            s_capture_storage_mounted ? "capture failed" : "sd card not mounted");
        return ret;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/captures");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, saved_name, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captures_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    httpd_resp_sendstr_chunk(req,
                             "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                             "<title>ESP32-P4 Captures</title>"
                             "<style>"
                             "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:20px;background:#f7f7f5;color:#1c1c1c}"
                             "header{display:flex;gap:12px;align-items:center;justify-content:space-between;flex-wrap:wrap}"
                             "a.button{background:#0b6bcb;color:white;padding:10px 14px;border-radius:6px;text-decoration:none}"
                             ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px;margin-top:18px}"
                             ".item{background:white;border:1px solid #ddd;border-radius:6px;padding:8px}"
                             "img{width:100%;height:auto;display:block;border-radius:4px}.name{font-size:13px;margin-top:6px;word-break:break-all}"
                             "</style></head><body><header><h1>Captures</h1><a class=\"button\" href=\"/capture\">Capture</a></header>");

    if (!s_capture_storage_mounted) {
        httpd_resp_sendstr_chunk(req, "<p>SD card is not mounted.</p></body></html>");
        return httpd_resp_sendstr_chunk(req, NULL);
    }

    DIR *dir = opendir(CAPTURE_DIR);
    if (dir == NULL) {
        httpd_resp_sendstr_chunk(req, "<p>Failed to open capture directory.</p></body></html>");
        return httpd_resp_sendstr_chunk(req, NULL);
    }

    httpd_resp_sendstr_chunk(req, "<div class=\"grid\">");
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_capture_filename(entry->d_name)) {
            continue;
        }

        char name[CAPTURE_FILENAME_MAX_LEN];
        memcpy(name, entry->d_name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        char item[256];
        snprintf(item,
                 sizeof(item),
                 "<a class=\"item\" href=\"/photo?name=%s\"><img src=\"/photo?name=%s\" loading=\"lazy\"><div class=\"name\">%s</div></a>",
                 name,
                 name,
                 name);
        httpd_resp_sendstr_chunk(req, item);
        count++;
    }
    closedir(dir);

    if (count == 0) {
        httpd_resp_sendstr_chunk(req, "</div><p>No captures yet.</p>");
    } else {
        httpd_resp_sendstr_chunk(req, "</div>");
    }

    httpd_resp_sendstr_chunk(req, "</body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t photo_handler(httpd_req_t *req)
{
    if (!s_capture_storage_mounted) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd card not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    char query[64] = {0};
    char name[CAPTURE_FILENAME_MAX_LEN] = {0};
    char path[CAPTURE_PATH_MAX_LEN] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        make_capture_path(name, path, sizeof(path)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid photo name");
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "photo not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char chunk[1024];
    esp_err_t ret = ESP_OK;
    while (!feof(file)) {
        size_t read_len = fread(chunk, 1, sizeof(chunk), file);
        if (read_len > 0) {
            ret = httpd_resp_send_chunk(req, chunk, read_len);
            if (ret != ESP_OK) {
                break;
            }
        }
    }

    fclose(file);
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }

    return ret;
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
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera busy");
        return ESP_ERR_TIMEOUT;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = s_queue_buf_type;
    buf.memory = MEMORY_TYPE;
    res = ioctl(wc->fd, VIDIOC_DQBUF, &buf);
    if (res != 0) {
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
    }
    xSemaphoreGive(wc->lock);

    if (res != ESP_OK) {
        ESP_LOGE(TAG, "classification failed (%s)", esp_err_to_name(res));
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

    esp_err_t sdio_ret = sdio_frame_tx_send_classification(jpeg_ptr, jpeg_len, &infer_result, total_ms);
    if (sdio_ret != ESP_OK) {
        ESP_LOGW(TAG, "SDIO frame send skipped/failed: %s", esp_err_to_name(sdio_ret));
    }

    res = httpd_resp_send_chunk(req, (const char *)jpeg_ptr, jpeg_len);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "send classify chunk failed");
        return res;
    }

    return httpd_resp_send_chunk(req, NULL, 0);
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
    httpd_uri_t capture_get_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
        .user_ctx = (void *)web_cam,
    };
    httpd_uri_t captures_get_uri = {
        .uri = "/captures",
        .method = HTTP_GET,
        .handler = captures_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t photo_get_uri = {
        .uri = "/photo",
        .method = HTTP_GET,
        .handler = photo_handler,
        .user_ctx = NULL,
    };

    ret = httpd_start(&video_web_httpd, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ret;
    }

    ESP_ERROR_CHECK(httpd_register_uri_handler(video_web_httpd, &pic_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(video_web_httpd, &record_file_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(video_web_httpd, &classify_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(video_web_httpd, &capture_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(video_web_httpd, &captures_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(video_web_httpd, &photo_get_uri));

    ESP_LOGI(TAG,
             "Starting HTTP server on port: '%d' (/pic, /record, /classify.jpg, /capture, /captures, /photo)",
             config.server_port);

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
static esp_err_t start_cam_web_server(int index, int cam_fd)
{
    web_cam_t *web_cam;
    esp_err_t ret = new_web_cam(cam_fd, &web_cam);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to new web cam");
        return ret;
    }
    app_capture_button_init(web_cam);
    return http_server_init(index, web_cam);
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

    s_capture_index_lock = xSemaphoreCreateMutex();
    if (s_capture_index_lock == NULL) {
        ESP_LOGE(TAG, "failed to create capture index lock");
        return;
    }

    /* Initialize the camera before network setup so sensors that depend on
     * early clock/power sequencing are detected right after boot.
     */
    app_wait_for_video_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(EXAMPLE_MDNS_HOST_NAME);

    ESP_ERROR_CHECK(app_wifi_softap_start());

    ret = app_capture_storage_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Capture storage disabled until SD card mount succeeds");
    }

    int video_cam_fd = app_video_open(CAM_DEV_PATH, EXAMPLE_VIDEO_FMT_RGB565);
    if (video_cam_fd < 0) {
        ESP_LOGE(TAG, "video cam open failed");
        return;
    }

    ESP_ERROR_CHECK(infer_bridge_init());
    ESP_ERROR_CHECK(start_cam_web_server(index, video_cam_fd));
    ESP_LOGI(TAG, "Example Start");
}
