#include "server_serial_time_adapter.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "mesh_image_gateway.h"
#include "server_serial_adapter.h"

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "server_pc_time"

#define RX_DRIVER_BUFFER_BYTES 512U
#define RX_READ_BYTES          128U
#define RX_TASK_STACK_BYTES    3072U
#define RX_TASK_PRIORITY       4U
#define UNIX_MS_MIN            UINT64_C(1704067200000) /* 2024-01-01 */
#define UNIX_MS_MAX            UINT64_C(4102444800000) /* 2100-01-01 */
#define PARSER_BUFFER_BYTES    (SERVER_SERIAL_TIME_PACKET_SIZE * 2U)

_Static_assert(offsetof(server_serial_time_packet_t, version) == 8U,
               "serial time version offset changed");
_Static_assert(offsetof(server_serial_time_packet_t, packet_size) == 10U,
               "serial time size offset changed");
_Static_assert(offsetof(server_serial_time_packet_t, unix_ms) == 12U,
               "serial time timestamp offset changed");
_Static_assert(offsetof(server_serial_time_packet_t, sequence) == 20U,
               "serial time sequence offset changed");
_Static_assert(offsetof(server_serial_time_packet_t, crc32) == 24U,
               "serial time CRC offset changed");

static portMUX_TYPE s_clock_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_synchronized;
static uint64_t s_anchor_unix_ms;
static int64_t s_anchor_monotonic_us;
static uint32_t s_invalid_packets;
static uint8_t s_parser_buffer[PARSER_BUFFER_BYTES];
static size_t s_parser_used;
static bool s_initialized;

static uint16_t get_le16(const uint8_t *source)
{
    return (uint16_t)source[0] | ((uint16_t)source[1] << 8);
}

static uint32_t get_le32(const uint8_t *source)
{
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
           ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
}

static uint64_t get_le64(const uint8_t *source)
{
    return (uint64_t)get_le32(source) |
           ((uint64_t)get_le32(source + 4U) << 32);
}

static bool laptop_clock_now(uint64_t *unix_ms, void *user_ctx)
{
    (void)user_ctx;
    if (unix_ms == NULL) {
        return false;
    }

    const int64_t now_us = esp_timer_get_time();
    uint64_t anchor_unix_ms;
    int64_t anchor_monotonic_us;
    bool synchronized;

    portENTER_CRITICAL(&s_clock_lock);
    synchronized = s_synchronized;
    anchor_unix_ms = s_anchor_unix_ms;
    anchor_monotonic_us = s_anchor_monotonic_us;
    portEXIT_CRITICAL(&s_clock_lock);

    if (!synchronized || now_us < anchor_monotonic_us) {
        return false;
    }
    const uint64_t age_us = (uint64_t)(now_us - anchor_monotonic_us);
    if (age_us > (uint64_t)CONFIG_SERVER_SERIAL_TIME_MAX_AGE_MS *
                     UINT64_C(1000)) {
        return false;
    }

    const uint64_t elapsed_ms = age_us / UINT64_C(1000);
    if (anchor_unix_ms > UINT64_MAX - elapsed_ms) {
        return false;
    }
    *unix_ms = anchor_unix_ms + elapsed_ms;
    return true;
}

static void accept_time(uint64_t unix_ms, uint32_t sequence)
{
    const int64_t now_us = esp_timer_get_time();
    bool was_fresh;

    portENTER_CRITICAL(&s_clock_lock);
    was_fresh = s_synchronized && now_us >= s_anchor_monotonic_us &&
                (uint64_t)(now_us - s_anchor_monotonic_us) <=
                    (uint64_t)CONFIG_SERVER_SERIAL_TIME_MAX_AGE_MS *
                        UINT64_C(1000);
    s_synchronized = true;
    s_anchor_unix_ms = unix_ms;
    s_anchor_monotonic_us = now_us;
    portEXIT_CRITICAL(&s_clock_lock);

    if (!was_fresh) {
        ESP_LOGI(TAG,
                 "laptop clock synchronized: epoch_ms=%" PRIu64
                 " sequence=%" PRIu32,
                 unix_ms, sequence);
    }
}

static bool decode_and_accept(const uint8_t *packet)
{
    if (memcmp(packet, SERVER_SERIAL_TIME_MAGIC,
               SERVER_SERIAL_TIME_MAGIC_SIZE) != 0 ||
        get_le16(packet + 8U) != SERVER_SERIAL_TIME_VERSION ||
        get_le16(packet + 10U) != SERVER_SERIAL_TIME_PACKET_SIZE) {
        return false;
    }

    const uint64_t unix_ms = get_le64(packet + 12U);
    const uint32_t sequence = get_le32(packet + 20U);
    const uint32_t expected_crc = get_le32(packet + 24U);
    const uint32_t calculated_crc = esp_crc32_le(
        0U, packet, SERVER_SERIAL_TIME_CRC_BYTES);
    if (unix_ms < UNIX_MS_MIN || unix_ms >= UNIX_MS_MAX || sequence == 0U ||
        calculated_crc != expected_crc) {
        return false;
    }

    if (mesh_image_gateway_is_receiving() ||
        server_serial_adapter_is_transmitting()) {
        return true;
    }

    accept_time(unix_ms, sequence);
    return true;
}

static void discard_front(size_t count)
{
    if (count >= s_parser_used) {
        s_parser_used = 0U;
        return;
    }
    memmove(s_parser_buffer, s_parser_buffer + count, s_parser_used - count);
    s_parser_used -= count;
}

static size_t find_magic(void)
{
    if (s_parser_used < SERVER_SERIAL_TIME_MAGIC_SIZE) {
        return SIZE_MAX;
    }
    for (size_t offset = 0U;
         offset + SERVER_SERIAL_TIME_MAGIC_SIZE <= s_parser_used; ++offset) {
        if (memcmp(s_parser_buffer + offset, SERVER_SERIAL_TIME_MAGIC,
                   SERVER_SERIAL_TIME_MAGIC_SIZE) == 0) {
            return offset;
        }
    }
    return SIZE_MAX;
}

static void keep_partial_magic(void)
{
    size_t keep = s_parser_used < SERVER_SERIAL_TIME_MAGIC_SIZE - 1U ?
                  s_parser_used : SERVER_SERIAL_TIME_MAGIC_SIZE - 1U;
    while (keep > 0U &&
           memcmp(s_parser_buffer + s_parser_used - keep,
                  SERVER_SERIAL_TIME_MAGIC, keep) != 0) {
        --keep;
    }
    discard_front(s_parser_used - keep);
}

static void process_parser(void)
{
    for (;;) {
        const size_t magic_at = find_magic();
        if (magic_at == SIZE_MAX) {
            keep_partial_magic();
            return;
        }
        if (magic_at > 0U) {
            discard_front(magic_at);
        }
        if (s_parser_used < SERVER_SERIAL_TIME_PACKET_SIZE) {
            return;
        }
        if (decode_and_accept(s_parser_buffer)) {
            discard_front(SERVER_SERIAL_TIME_PACKET_SIZE);
            continue;
        }

        ++s_invalid_packets;
        if (s_invalid_packets <= 3U ||
            (s_invalid_packets & (s_invalid_packets - 1U)) == 0U) {
            ESP_LOGW(TAG, "discarded invalid laptop-time packet count=%" PRIu32,
                     s_invalid_packets);
        }
        discard_front(1U);
    }
}

static void feed_parser(const uint8_t *data, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        if (s_parser_used == sizeof(s_parser_buffer)) {
            discard_front(1U);
        }
        s_parser_buffer[s_parser_used++] = data[index];
        process_parser();
    }
}

static void serial_time_rx_task(void *argument)
{
    const uart_port_t port = (uart_port_t)(intptr_t)argument;
    uint8_t input[RX_READ_BYTES];

    for (;;) {
        const int received = uart_read_bytes(
            port, input, sizeof(input), pdMS_TO_TICKS(1000U));
        if (received > 0) {
            feed_parser(input, (size_t)received);
        } else if (received < 0) {
            ESP_LOGW(TAG, "console UART read failed: %d", received);
            vTaskDelay(pdMS_TO_TICKS(100U));
        }
    }
}

esp_err_t server_serial_time_adapter_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const uart_port_t port = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
    if (!uart_is_driver_installed(port)) {
        esp_err_t err = uart_driver_install(
            port, RX_DRIVER_BUFFER_BYTES, 0U, 0U, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }
    }

    uart_vfs_dev_use_driver(port);
    if (uart_vfs_dev_port_set_tx_line_endings(port, ESP_LINE_ENDINGS_LF) != 0) {
        return ESP_FAIL;
    }

    esp_err_t err = mesh_image_gateway_set_time_provider(
        laptop_clock_now, NULL);
    if (err != ESP_OK) {
        return err;
    }
    BaseType_t created = xTaskCreate(
        serial_time_rx_task, "pc_time_rx", RX_TASK_STACK_BYTES,
        (void *)(intptr_t)port, RX_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "waiting for laptop time on console UART%d: magic=%s max_age=%d ms",
             (int)port, SERVER_SERIAL_TIME_MAGIC,
             CONFIG_SERVER_SERIAL_TIME_MAX_AGE_MS);
    return ESP_OK;
}
