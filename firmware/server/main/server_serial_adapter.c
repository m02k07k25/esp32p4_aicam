#include "server_serial_adapter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "ble_mesh_image_protocol.h"
#include "mesh_image_gateway.h"

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TAG "server_serial"

#define SERIAL_SLOT_COUNT       1U
#define SERIAL_TASK_STACK_BYTES 4096U
#define SERIAL_TASK_PRIORITY    4U

#if !CONFIG_LIBC_STDOUT_LINE_ENDING_LF
#error "SERVER_SERIAL_IMAGE_ENABLE requires LIBC stdout LF (no CRLF conversion)"
#endif

_Static_assert(offsetof(server_serial_frame_header_t, version) == 8U,
               "serial version offset changed");
_Static_assert(offsetof(server_serial_frame_header_t, source_addr) == 12U,
               "serial source offset changed");
_Static_assert(offsetof(server_serial_frame_header_t, event_time_ms) == 16U,
               "serial event-time offset changed");
_Static_assert(offsetof(server_serial_frame_header_t, jpeg_len) == 24U,
               "serial JPEG length offset changed");
_Static_assert(offsetof(server_serial_frame_header_t, sequence) == 32U,
               "serial sequence offset changed");
_Static_assert(offsetof(server_serial_frame_header_t, header_crc32) == 36U,
               "serial header CRC offset changed");

typedef struct {
    uint16_t source_addr;
    server_time_source_t time_source;
    uint64_t event_time_ms;
    uint32_t jpeg_len;
    uint32_t sequence;
    uint8_t jpeg[BLE_MESH_IMAGE_MAX_BYTES];
} serial_image_slot_t;

/*
 * Keep the 30 KiB transfer slot out of .dram0.bss. Wi-Fi, Bluetooth Mesh,
 * and the fixed image reassembly buffer otherwise overflow ESP32's link-time
 * DRAM region when serial output and SNTP are enabled together. This is one
 * bounded allocation during startup; the completion callback still performs
 * no allocation and never blocks.
 */
static serial_image_slot_t *s_slots;
static QueueHandle_t s_free_slots;
static QueueHandle_t s_pending_slots;
static StaticQueue_t s_free_queue_control;
static StaticQueue_t s_pending_queue_control;
static uint8_t s_free_queue_storage[
    SERIAL_SLOT_COUNT * sizeof(serial_image_slot_t *)];
static uint8_t s_pending_queue_storage[
    SERIAL_SLOT_COUNT * sizeof(serial_image_slot_t *)];
static uint32_t s_next_sequence = 1U;
static portMUX_TYPE s_drop_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_dropped_images;

static void record_dropped_image(void)
{
    portENTER_CRITICAL(&s_drop_lock);
    ++s_dropped_images;
    portEXIT_CRITICAL(&s_drop_lock);
}

static uint32_t take_dropped_images(void)
{
    portENTER_CRITICAL(&s_drop_lock);
    uint32_t count = s_dropped_images;
    s_dropped_images = 0U;
    portEXIT_CRITICAL(&s_drop_lock);
    return count;
}

static void put_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *destination, uint64_t value)
{
    put_le32(destination, (uint32_t)value);
    put_le32(destination + 4, (uint32_t)(value >> 32));
}

static void encode_header(uint8_t header[SERVER_SERIAL_FRAME_HEADER_SIZE],
                          const serial_image_slot_t *slot,
                          uint32_t jpeg_crc32)
{
    memset(header, 0, SERVER_SERIAL_FRAME_HEADER_SIZE);
    memcpy(header, SERVER_SERIAL_FRAME_MAGIC,
           SERVER_SERIAL_FRAME_MAGIC_SIZE);
    put_le16(header + 8, SERVER_SERIAL_FRAME_VERSION);
    put_le16(header + 10, SERVER_SERIAL_FRAME_HEADER_SIZE);
    put_le16(header + 12, slot->source_addr);
    header[14] = (uint8_t)slot->time_source;
    header[15] = 0U;
    put_le64(header + 16, slot->event_time_ms);
    put_le32(header + 24, slot->jpeg_len);
    put_le32(header + 28, jpeg_crc32);
    put_le32(header + 32, slot->sequence);
    put_le32(header + 36,
             esp_crc32_le(0U, header,
                          SERVER_SERIAL_FRAME_HEADER_SIZE - sizeof(uint32_t)));
}

static esp_err_t write_record(const serial_image_slot_t *slot)
{
    uint8_t header[SERVER_SERIAL_FRAME_HEADER_SIZE];
    uint32_t jpeg_crc32 = esp_crc32_le(0U, slot->jpeg, slot->jpeg_len);
    encode_header(header, slot, jpeg_crc32);

    /* ESP-IDF's text logger uses the same FILE lock, so no log bytes can be
     * inserted between magic, header, and JPEG while this lock is held. */
    flockfile(stdout);
    size_t header_written = fwrite(header, 1U, sizeof(header), stdout);
    size_t jpeg_written = fwrite(slot->jpeg, 1U, slot->jpeg_len, stdout);
    int flush_result = fflush(stdout);
    bool write_failed = header_written != sizeof(header) ||
                        jpeg_written != slot->jpeg_len ||
                        flush_result != 0;
    if (write_failed) {
        clearerr(stdout);
    }
    funlockfile(stdout);
    return write_failed ? ESP_FAIL : ESP_OK;
}

static const char *time_source_name(server_time_source_t source);

static void serial_writer_task(void *argument)
{
    (void)argument;
    for (;;) {
        serial_image_slot_t *slot = NULL;
        if (xQueueReceive(s_pending_slots, &slot, portMAX_DELAY) != pdTRUE ||
            slot == NULL) {
            continue;
        }

        esp_err_t err = write_record(slot);
        uint32_t dropped = take_dropped_images();
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "console JPEG sent seq=%lu id=%u src=0x%04x event_ms=%llu "
                     "time_source=%s bytes=%lu dropped_since_last=%lu",
                     (unsigned long)slot->sequence,
                     mesh_image_gateway_device_id_from_addr(slot->source_addr),
                     slot->source_addr,
                     (unsigned long long)slot->event_time_ms,
                     time_source_name(slot->time_source),
                     (unsigned long)slot->jpeg_len, (unsigned long)dropped);
        } else {
            ESP_LOGE(TAG,
                     "console JPEG failed seq=%lu id=%u src=0x%04x event_ms=%llu "
                     "bytes=%lu dropped_since_last=%lu: %s",
                     (unsigned long)slot->sequence,
                     mesh_image_gateway_device_id_from_addr(slot->source_addr),
                     slot->source_addr,
                     (unsigned long long)slot->event_time_ms,
                     (unsigned long)slot->jpeg_len, (unsigned long)dropped,
                     esp_err_to_name(err));
        }

        if (xQueueSend(s_free_slots, &slot, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "static serial slot return failed");
        }
    }
}

static const char *time_source_name(server_time_source_t source)
{
    switch (source) {
    case SERVER_TIME_P4_DETECTED:
        return "p4_detected";
    case SERVER_TIME_RX_ESTIMATE:
        return "rx_estimate";
    default:
        return "unknown";
    }
}

static void image_complete(const server_image_t *image, void *user_ctx)
{
    (void)user_ctx;
    if (image == NULL || image->jpeg == NULL || image->jpeg_len == 0U ||
        image->jpeg_len > BLE_MESH_IMAGE_MAX_BYTES) {
        record_dropped_image();
        return;
    }

    serial_image_slot_t *slot = NULL;
    if (xQueueReceive(s_free_slots, &slot, 0) != pdTRUE || slot == NULL) {
        record_dropped_image();
        return;
    }

    slot->source_addr = image->source_addr;
    slot->time_source = image->time_source;
    slot->event_time_ms = image->event_time_ms;
    slot->jpeg_len = (uint32_t)image->jpeg_len;
    uint32_t sequence = s_next_sequence++;
    slot->sequence = sequence;
    if (s_next_sequence == 0U) {
        s_next_sequence = 1U;
    }
    memcpy(slot->jpeg, image->jpeg, image->jpeg_len);

    if (xQueueSend(s_pending_slots, &slot, 0) != pdTRUE) {
        (void)xQueueSend(s_free_slots, &slot, 0);
        record_dropped_image();
        return;
    }
}

esp_err_t server_serial_adapter_init(void)
{
    const uint32_t slot_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t free_before = heap_caps_get_free_size(slot_caps);
    size_t largest_before = heap_caps_get_largest_free_block(slot_caps);
    s_slots = heap_caps_calloc(SERIAL_SLOT_COUNT, sizeof(*s_slots), slot_caps);
    if (s_slots == NULL) {
        ESP_LOGE(TAG,
                 "serial slot allocation failed: bytes=%u free=%u largest=%u",
                 (unsigned)(SERIAL_SLOT_COUNT * sizeof(*s_slots)),
                 (unsigned)free_before, (unsigned)largest_before);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "serial slots preallocated: bytes=%u internal_free=%u->%u "
             "largest=%u->%u",
             (unsigned)(SERIAL_SLOT_COUNT * sizeof(*s_slots)),
             (unsigned)free_before,
             (unsigned)heap_caps_get_free_size(slot_caps),
             (unsigned)largest_before,
             (unsigned)heap_caps_get_largest_free_block(slot_caps));

    s_free_slots = xQueueCreateStatic(
        SERIAL_SLOT_COUNT, sizeof(serial_image_slot_t *),
        s_free_queue_storage, &s_free_queue_control);
    s_pending_slots = xQueueCreateStatic(
        SERIAL_SLOT_COUNT, sizeof(serial_image_slot_t *),
        s_pending_queue_storage, &s_pending_queue_control);
    if (s_free_slots == NULL || s_pending_slots == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t index = 0; index < SERIAL_SLOT_COUNT; ++index) {
        serial_image_slot_t *slot = &s_slots[index];
        if (xQueueSend(s_free_slots, &slot, 0) != pdTRUE) {
            return ESP_FAIL;
        }
    }

    BaseType_t created = xTaskCreate(serial_writer_task, "jpeg_stdout",
                                     SERIAL_TASK_STACK_BYTES, NULL,
                                     SERIAL_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = mesh_image_gateway_register_image_callback(
        image_complete, NULL);
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_ESP_CONSOLE_UART
    ESP_LOGI(TAG,
             "binary JPEG records on console stdout: %d baud, %u slots",
             CONFIG_ESP_CONSOLE_UART_BAUDRATE, SERIAL_SLOT_COUNT);
#else
    ESP_LOGI(TAG,
             "binary JPEG records on non-UART console stdout (%u slots)",
             SERIAL_SLOT_COUNT);
#endif
    return ESP_OK;
}
