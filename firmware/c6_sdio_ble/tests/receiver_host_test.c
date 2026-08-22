#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Include the production receiver in this translation unit. Its static RX
 * callbacks and state helpers remain private to this executable, while the
 * ESP-IDF dependencies are supplied by tests/stubs.
 */
#include "../main/sdio_frame_receiver.c"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define TEST_QUEUE_CAPACITY 64U
#define TEST_QUEUE_ITEM_MAX 128U
#define TEST_CONTROL_CAPACITY 64U
#define TEST_TIME_CAPACITY 16U
#define TEST_BLE_FRAME_ID UINT16_C(0x2345)
#define TEST_DETECTED_AT_MS UINT64_C(1760000123456)

typedef struct {
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t count;
    uint8_t items[TEST_QUEUE_CAPACITY][TEST_QUEUE_ITEM_MAX];
} test_queue_t;

static test_queue_t g_queue;
static test_queue_t g_lifecycle_queue;
static int64_t g_now_us;
static bool g_mesh_ready;
static bool g_mesh_busy;
static esp_err_t g_submit_result;
static esp_err_t g_time_submit_result;
static const uint8_t *g_submitted_jpeg;
static size_t g_submitted_size;
static uint32_t g_submitted_p4_frame_id;
static uint64_t g_submitted_detected_at_ms;
static uint32_t g_submitted_jpeg_crc32;
static uint32_t g_submitted_time_request_id;
static uint64_t g_submitted_client_tx_us;
static sdio_frame_control_t g_controls[TEST_CONTROL_CAPACITY];
static size_t g_control_count;
static sdio_time_message_t g_time_samples[TEST_TIME_CAPACITY];
static size_t g_time_sample_count;

#define EXPECT(expression)                                                       \
    do {                                                                         \
        if (!(expression)) {                                                     \
            fprintf(stderr, "  assertion failed at line %d: %s\n",             \
                    __LINE__, #expression);                                      \
            return false;                                                        \
        }                                                                        \
    } while (0)

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "host-test-error";
}

void host_test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buffer, uint32_t length)
{
    crc = ~crc;
    for (uint32_t byte = 0; byte < length; ++byte) {
        crc ^= buffer[byte];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}

int64_t esp_timer_get_time(void)
{
    return g_now_us;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    (void)length;
    if (item_size > TEST_QUEUE_ITEM_MAX) {
        return NULL;
    }
    memset(&g_queue, 0, sizeof(g_queue));
    g_queue.item_size = item_size;
    g_queue.capacity = length < TEST_QUEUE_CAPACITY
                           ? length
                           : TEST_QUEUE_CAPACITY;
    return &g_queue;
}

BaseType_t xQueueSend(QueueHandle_t queue,
                      const void *item,
                      TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    test_queue_t *test_queue = queue;
    if (test_queue == NULL || item == NULL ||
        test_queue->item_size > TEST_QUEUE_ITEM_MAX ||
        test_queue->capacity == 0 ||
        test_queue->count == test_queue->capacity) {
        return pdFALSE;
    }

    const size_t tail = (test_queue->head + test_queue->count) %
                        test_queue->capacity;
    memcpy(test_queue->items[tail], item, test_queue->item_size);
    test_queue->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue,
                         void *item,
                         TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    test_queue_t *test_queue = queue;
    if (test_queue == NULL || item == NULL || test_queue->count == 0) {
        return pdFALSE;
    }

    memcpy(item, test_queue->items[test_queue->head], test_queue->item_size);
    test_queue->head = (test_queue->head + 1U) % test_queue->capacity;
    test_queue->count--;
    return pdTRUE;
}

BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item)
{
    test_queue_t *test_queue = queue;
    if (test_queue == NULL || item == NULL || test_queue->capacity == 0 ||
        test_queue->item_size > TEST_QUEUE_ITEM_MAX) {
        return pdFALSE;
    }
    if (test_queue->count == test_queue->capacity) {
        memcpy(test_queue->items[test_queue->head], item, test_queue->item_size);
        return pdTRUE;
    }
    return xQueueSend(queue, item, 0);
}

void vQueueDelete(QueueHandle_t queue)
{
    (void)queue;
}

BaseType_t xTaskCreate(TaskFunction_t task,
                       const char *name,
                       uint32_t stack_depth,
                       void *argument,
                       UBaseType_t priority,
                       TaskHandle_t *created_task)
{
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)argument;
    (void)priority;
    if (created_task != NULL) {
        *created_task = (TaskHandle_t)(uintptr_t)1;
    }
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

esp_err_t esp_hosted_send_custom_data(uint32_t msg_id,
                                      const uint8_t *data,
                                      size_t data_len)
{
    if (data == NULL) {
        return ESP_FAIL;
    }
    if (msg_id == SDIO_FRAME_CONTROL_MSG_ID &&
        data_len == sizeof(sdio_frame_control_t) &&
        g_control_count < TEST_CONTROL_CAPACITY) {
        memcpy(&g_controls[g_control_count], data, data_len);
        g_control_count++;
        return ESP_OK;
    }
    if (msg_id == SDIO_TIME_MSG_ID &&
        data_len == sizeof(sdio_time_message_t) &&
        g_time_sample_count < TEST_TIME_CAPACITY) {
        memcpy(&g_time_samples[g_time_sample_count], data, data_len);
        g_time_sample_count++;
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t esp_hosted_register_custom_callback(
    uint32_t msg_id,
    void (*callback)(uint32_t msg_id, const uint8_t *data, size_t data_len))
{
    (void)msg_id;
    (void)callback;
    return ESP_OK;
}

esp_err_t ble_mesh_image_source_register_callbacks(
    const ble_mesh_image_source_callbacks_t *callbacks,
    void *user_ctx)
{
    (void)callbacks;
    (void)user_ctx;
    return ESP_OK;
}

esp_err_t ble_mesh_image_source_init(void)
{
    return ESP_OK;
}

esp_err_t ble_mesh_image_source_submit(const uint8_t *jpeg,
                                       size_t len,
                                       uint32_t p4_frame_id,
                                       uint64_t detected_at_ms,
                                       uint32_t jpeg_crc32,
                                       uint16_t *ble_frame_id)
{
    g_submitted_jpeg = jpeg;
    g_submitted_size = len;
    g_submitted_p4_frame_id = p4_frame_id;
    g_submitted_detected_at_ms = detected_at_ms;
    g_submitted_jpeg_crc32 = jpeg_crc32;
    if (g_submit_result == ESP_OK) {
        g_mesh_busy = true;
        if (ble_frame_id != NULL) {
            *ble_frame_id = TEST_BLE_FRAME_ID;
        }
    }
    return g_submit_result;
}

esp_err_t ble_mesh_image_source_request_time(
    uint32_t request_id, uint64_t client_tx_monotonic_us)
{
    g_submitted_time_request_id = request_id;
    g_submitted_client_tx_us = client_tx_monotonic_us;
    if (g_time_submit_result == ESP_OK) {
        g_mesh_busy = true;
    }
    return g_time_submit_result;
}

bool ble_mesh_image_source_is_ready(void)
{
    return g_mesh_ready;
}

bool ble_mesh_image_source_is_busy(void)
{
    return g_mesh_busy;
}

void ble_mesh_image_source_ready_changed(bool ready)
{
    (void)ready;
}

void ble_mesh_image_source_frame_done(uint32_t p4_frame_id,
                                      uint16_t ble_frame_id,
                                      esp_err_t status)
{
    (void)p4_frame_id;
    (void)ble_frame_id;
    (void)status;
}

static void reset_fixture(void)
{
    memset(&g_queue, 0, sizeof(g_queue));
    g_queue.item_size = sizeof(worker_event_t);
    g_queue.capacity = TEST_QUEUE_CAPACITY;
    memset(&g_lifecycle_queue, 0, sizeof(g_lifecycle_queue));
    g_lifecycle_queue.item_size = sizeof(worker_event_t);
    g_lifecycle_queue.capacity = 1;
    g_now_us = 1;
    g_mesh_ready = true;
    g_mesh_busy = false;
    g_submit_result = ESP_OK;
    g_time_submit_result = ESP_OK;
    g_submitted_jpeg = NULL;
    g_submitted_size = 0;
    g_submitted_p4_frame_id = 0;
    g_submitted_detected_at_ms = 0;
    g_submitted_jpeg_crc32 = 0;
    g_submitted_time_request_id = 0U;
    g_submitted_client_tx_us = 0U;
    memset(g_controls, 0, sizeof(g_controls));
    g_control_count = 0;
    memset(g_time_samples, 0, sizeof(g_time_samples));
    g_time_sample_count = 0U;
    s_worker_queue = &g_queue;
    s_lifecycle_queue = &g_lifecycle_queue;
    s_mesh_ready_event_pending = false;
    reset_receiver_locked();
    memset(&s_last_terminal, 0, sizeof(s_last_terminal));
}

static bool process_next_event(void)
{
    worker_event_t event;
    if (xQueueReceive(s_lifecycle_queue, &event, 0) == pdTRUE) {
        if (event.kind == WORK_FRAME_COMPLETE) {
            submit_completed_frame(event.frame_id);
        } else if (event.kind == WORK_BLE_FRAME_DONE) {
            handle_ble_frame_done(&event);
        } else if (event.kind == WORK_BLE_TIME_DONE) {
            handle_ble_time_done(&event);
        } else {
            return false;
        }
        return true;
    }

    if (s_mesh_ready_event_pending) {
        s_mesh_ready_event_pending = false;
        reply_to_query(0);
        return true;
    }

    if (xQueueReceive(s_worker_queue, &event, 0) != pdTRUE) {
        return false;
    }
    switch (event.kind) {
    case WORK_QUERY:
        reply_to_query(event.frame_id);
        break;
    case WORK_SEND_CONTROL:
        (void)send_control(event.control_state,
                           event.frame_id,
                           event.ble_frame_id,
                           event.reason,
                           event.detail);
        break;
    case WORK_SEND_TIME_SAMPLE:
        (void)send_time_sample(&event.time);
        break;
    default:
        return false;
    }
    return true;
}

static void fill_jpeg(uint8_t *jpeg, size_t size)
{
    for (size_t index = 0; index < size; ++index) {
        jpeg[index] = (uint8_t)((index * 37U + 11U) & 0xffU);
    }
    if (size >= 4) {
        jpeg[0] = 0xff;
        jpeg[1] = 0xd8;
        jpeg[size - 2] = 0xff;
        jpeg[size - 1] = 0xd9;
    }
}

static sdio_frame_chunk_header_t make_header(uint32_t frame_id,
                                              uint16_t chunk_index,
                                              uint16_t chunk_count,
                                              uint32_t chunk_offset,
                                              uint32_t chunk_size,
                                              uint32_t jpeg_size,
                                              uint32_t crc)
{
    sdio_frame_chunk_header_t header = {
        .magic = SDIO_FRAME_MAGIC,
        .version = SDIO_FRAME_VERSION,
        .header_size = sizeof(sdio_frame_chunk_header_t),
        .frame_id = frame_id,
        .chunk_index = chunk_index,
        .chunk_count = chunk_count,
        .flags = 0,
        .chunk_offset = chunk_offset,
        .chunk_size = chunk_size,
        .jpeg_size = jpeg_size,
        .detected_at_ms = TEST_DETECTED_AT_MS,
        .jpeg_crc32 = crc,
    };
    if (chunk_index == 0) {
        header.flags |= SDIO_FRAME_FLAG_FIRST_CHUNK;
    }
    if (chunk_index == chunk_count - 1U) {
        header.flags |= SDIO_FRAME_FLAG_LAST_CHUNK;
    }
    return header;
}

static void dispatch_chunk(const sdio_frame_chunk_header_t *header,
                           const uint8_t *payload)
{
    uint8_t packet[sizeof(sdio_frame_chunk_header_t) +
                   SDIO_FRAME_CHUNK_DATA_MAX];
    memcpy(packet, header, sizeof(*header));
    memcpy(packet + sizeof(*header), payload, header->chunk_size);
    frame_chunk_callback(SDIO_FRAME_MSG_ID,
                         packet,
                         sizeof(*header) + header->chunk_size);
}

static void dispatch_query(uint32_t frame_id)
{
    const sdio_frame_control_t query = {
        .magic = SDIO_FRAME_CONTROL_MAGIC,
        .version = SDIO_FRAME_VERSION,
        .size = sizeof(sdio_frame_control_t),
        .state = SDIO_FRAME_CONTROL_QUERY,
        .frame_id = frame_id,
    };
    control_callback(SDIO_FRAME_CONTROL_MSG_ID,
                     (const uint8_t *)&query,
                      sizeof(query));
}

static void dispatch_time_query(uint32_t request_id, uint64_t client_tx_us)
{
    const sdio_time_message_t query = {
        .magic = SDIO_TIME_MAGIC,
        .version = SDIO_FRAME_VERSION,
        .size = sizeof(sdio_time_message_t),
        .kind = SDIO_TIME_KIND_QUERY,
        .status = SDIO_TIME_STATUS_NONE,
        .request_id = request_id,
        .client_tx_monotonic_us = client_tx_us,
    };
    time_query_callback(SDIO_TIME_MSG_ID,
                        (const uint8_t *)&query,
                        sizeof(query));
}

static bool expect_control(size_t index,
                           sdio_frame_control_state_t state,
                           sdio_frame_control_reason_t reason,
                           uint32_t frame_id)
{
    EXPECT(index < g_control_count);
    EXPECT(g_controls[index].magic == SDIO_FRAME_CONTROL_MAGIC);
    EXPECT(g_controls[index].version == SDIO_FRAME_VERSION);
    EXPECT(g_controls[index].size == sizeof(sdio_frame_control_t));
    EXPECT(g_controls[index].state == (uint16_t)state);
    EXPECT(g_controls[index].reason == (uint16_t)reason);
    EXPECT(g_controls[index].frame_id == frame_id);
    return true;
}

static bool expect_time_sample(size_t index,
                               sdio_time_status_t status,
                               uint32_t request_id,
                               uint64_t client_tx_us)
{
    EXPECT(index < g_time_sample_count);
    const sdio_time_message_t *sample = &g_time_samples[index];
    EXPECT(sample->magic == SDIO_TIME_MAGIC);
    EXPECT(sample->version == SDIO_FRAME_VERSION);
    EXPECT(sample->size == sizeof(*sample));
    EXPECT(sample->kind == SDIO_TIME_KIND_SAMPLE);
    EXPECT(sample->status == (uint16_t)status);
    EXPECT(sample->request_id == request_id);
    EXPECT(sample->client_tx_monotonic_us == client_tx_us);
    return true;
}

static bool test_30720_boundary_and_ble_lifecycle(void)
{
    static uint8_t jpeg[SDIO_FRAME_MAX_JPEG_SIZE];
    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const uint16_t chunk_count = (uint16_t)(
        (sizeof(jpeg) + SDIO_FRAME_CHUNK_DATA_MAX - 1U) /
        SDIO_FRAME_CHUNK_DATA_MAX);

    uint32_t offset = 0;
    for (uint16_t index = 0; index < chunk_count; ++index) {
        const uint32_t remaining = (uint32_t)sizeof(jpeg) - offset;
        const uint32_t chunk_size =
            remaining < SDIO_FRAME_CHUNK_DATA_MAX
                ? remaining
                : SDIO_FRAME_CHUNK_DATA_MAX;
        const sdio_frame_chunk_header_t header =
            make_header(1001, index, chunk_count, offset, chunk_size,
                        sizeof(jpeg), crc);
        dispatch_chunk(&header, jpeg + offset);
        offset += chunk_size;
    }

    EXPECT(offset == SDIO_FRAME_MAX_JPEG_SIZE);
    EXPECT(s_receiver.state == RECEIVER_PENDING_BLE);
    EXPECT(g_lifecycle_queue.count == 1);
    EXPECT(process_next_event());
    EXPECT(s_receiver.state == RECEIVER_BLE_ACTIVE);
    EXPECT(g_submitted_jpeg == s_jpeg_buffer);
    EXPECT(g_submitted_size == sizeof(jpeg));
    EXPECT(g_submitted_p4_frame_id == 1001);
    EXPECT(g_submitted_detected_at_ms == TEST_DETECTED_AT_MS);
    EXPECT(g_submitted_jpeg_crc32 == crc);
    EXPECT(memcmp(g_submitted_jpeg, jpeg, sizeof(jpeg)) == 0);
    EXPECT(g_control_count == 1);
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_ACCEPTED,
                          SDIO_FRAME_REASON_NONE, 1001));
    EXPECT(g_controls[0].ble_frame_id == TEST_BLE_FRAME_ID);
    EXPECT(g_controls[0].detail == SDIO_FRAME_MAX_JPEG_SIZE);

    dispatch_query(2002);
    EXPECT(process_next_event());
    EXPECT(g_control_count == 2);
    EXPECT(expect_control(1, SDIO_FRAME_CONTROL_BUSY,
                          SDIO_FRAME_REASON_BUSY, 2002));
    EXPECT(g_controls[1].ble_frame_id == TEST_BLE_FRAME_ID);

    g_mesh_busy = false;
    mesh_frame_done_callback(1001, TEST_BLE_FRAME_ID, ESP_OK, NULL);
    EXPECT(process_next_event());
    EXPECT(s_receiver.state == RECEIVER_IDLE);
    EXPECT(g_control_count == 4);
    EXPECT(expect_control(2, SDIO_FRAME_CONTROL_SERVER_ACKED,
                          SDIO_FRAME_REASON_NONE, 1001));
    EXPECT(expect_control(3, SDIO_FRAME_CONTROL_READY,
                          SDIO_FRAME_REASON_NONE, 0));
    EXPECT(g_controls[3].detail == SDIO_FRAME_MAX_JPEG_SIZE);

    dispatch_query(1001);
    EXPECT(process_next_event());
    EXPECT(g_control_count == 5);
    EXPECT(expect_control(4, SDIO_FRAME_CONTROL_SERVER_ACKED,
                          SDIO_FRAME_REASON_NONE, 1001));
    EXPECT(g_controls[4].ble_frame_id == TEST_BLE_FRAME_ID);
    return true;
}

static bool test_30721_is_rejected(void)
{
    uint8_t payload[SDIO_FRAME_CHUNK_DATA_MAX] = {0xff, 0xd8};
    reset_fixture();
    sdio_frame_chunk_header_t header =
        make_header(1002, 0, 5, 0, sizeof(payload),
                    SDIO_FRAME_MAX_JPEG_SIZE + 1U, 0);
    dispatch_chunk(&header, payload);
    EXPECT(process_next_event());
    EXPECT(g_control_count == 1);
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_SIZE, 1002));
    EXPECT(s_receiver.state == RECEIVER_IDLE);
    return true;
}

/*
 * Image dimensions remain inside the JPEG bitstream; protocol v3 and the BLE
 * Gateway packets intentionally carry only the encoded byte count.  Model a
 * P4-produced baseline SOF0 marker and verify that the production receiver
 * preserves the complete 224x224 JPEG byte stream for its zero-copy BLE handoff.
 */
static bool test_224x224_jpeg_is_forwarded_opaquely(void)
{
    enum { TEST_JPEG_SIZE = 1024 };
    static uint8_t jpeg[TEST_JPEG_SIZE];
    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));

    /* SOF0: marker, segment length, precision, height, width, components. */
    const uint8_t sof0[] = {
        0xff, 0xc0, 0x00, 0x11, 0x08,
        0x00, 0xe0, /* height = 224 */
        0x00, 0xe0, /* width = 224 */
        0x03,
        0x01, 0x11, 0x00,
        0x02, 0x11, 0x01,
        0x03, 0x11, 0x01,
    };
    memcpy(jpeg + 2U, sof0, sizeof(sof0));

    EXPECT((((uint16_t)jpeg[7] << 8) | jpeg[8]) == 224U);
    EXPECT((((uint16_t)jpeg[9] << 8) | jpeg[10]) == 224U);

    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const sdio_frame_chunk_header_t header =
        make_header(1003, 0, 1, 0, sizeof(jpeg), sizeof(jpeg), crc);
    dispatch_chunk(&header, jpeg);

    EXPECT(s_receiver.state == RECEIVER_PENDING_BLE);
    EXPECT(process_next_event());
    EXPECT(s_receiver.state == RECEIVER_BLE_ACTIVE);
    EXPECT(g_submitted_jpeg == s_jpeg_buffer);
    EXPECT(g_submitted_size == sizeof(jpeg));
    EXPECT(g_submitted_p4_frame_id == 1003);
    EXPECT(g_submitted_detected_at_ms == TEST_DETECTED_AT_MS);
    EXPECT(g_submitted_jpeg_crc32 == crc);
    EXPECT(memcmp(g_submitted_jpeg, jpeg, sizeof(jpeg)) == 0);
    EXPECT(g_control_count == 1);
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_ACCEPTED,
                          SDIO_FRAME_REASON_NONE, 1003));
    EXPECT(g_controls[0].detail == sizeof(jpeg));
    return true;
}

static bool test_reverse_chunk_is_rejected(void)
{
    uint8_t jpeg[10];
    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const sdio_frame_chunk_header_t second =
        make_header(1100, 1, 2, 5, 5, sizeof(jpeg), crc);
    dispatch_chunk(&second, jpeg + 5);
    EXPECT(process_next_event());
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_ORDER, 1100));
    EXPECT(s_receiver.state == RECEIVER_IDLE);
    return true;
}

static bool test_missing_chunk_is_rejected(void)
{
    uint8_t jpeg[15];
    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const sdio_frame_chunk_header_t first =
        make_header(1101, 0, 3, 0, 5, sizeof(jpeg), crc);
    const sdio_frame_chunk_header_t third =
        make_header(1101, 2, 3, 10, 5, sizeof(jpeg), crc);
    dispatch_chunk(&first, jpeg);
    dispatch_chunk(&third, jpeg + 10);
    EXPECT(process_next_event());
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_ORDER, 1101));
    EXPECT(s_receiver.state == RECEIVER_IDLE);
    return true;
}

static bool test_duplicate_chunk_is_rejected(void)
{
    uint8_t jpeg[20];
    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const sdio_frame_chunk_header_t first =
        make_header(1102, 0, 4, 0, 5, sizeof(jpeg), crc);
    const sdio_frame_chunk_header_t second =
        make_header(1102, 1, 4, 5, 5, sizeof(jpeg), crc);
    dispatch_chunk(&first, jpeg);
    dispatch_chunk(&second, jpeg + 5);
    dispatch_chunk(&second, jpeg + 5);
    EXPECT(process_next_event());
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_ORDER, 1102));
    EXPECT(s_receiver.state == RECEIVER_IDLE);
    return true;
}

static bool test_metadata_mismatch_is_rejected(void)
{
    uint8_t jpeg[10];
    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const sdio_frame_chunk_header_t first =
        make_header(1103, 0, 2, 0, 5, sizeof(jpeg), crc);
    sdio_frame_chunk_header_t second =
        make_header(1103, 1, 2, 5, 5, sizeof(jpeg), crc);
    second.detected_at_ms++;
    dispatch_chunk(&first, jpeg);
    dispatch_chunk(&second, jpeg + 5);
    EXPECT(process_next_event());
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_ORDER, 1103));
    EXPECT(s_receiver.state == RECEIVER_IDLE);
    return true;
}

static bool test_crc_error_is_rejected(void)
{
    uint8_t jpeg[100];
    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t wrong_crc = esp_crc32_le(0, jpeg, sizeof(jpeg)) ^ 1U;
    const sdio_frame_chunk_header_t header =
        make_header(1200, 0, 1, 0, sizeof(jpeg), sizeof(jpeg), wrong_crc);
    dispatch_chunk(&header, jpeg);
    EXPECT(process_next_event());
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_CRC, 1200));
    EXPECT(s_receiver.state == RECEIVER_IDLE);
    return true;
}

static bool test_jpeg_soi_and_eoi_are_required(void)
{
    uint8_t jpeg[100];

    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));
    jpeg[0] = 0;
    uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    sdio_frame_chunk_header_t header =
        make_header(1201, 0, 1, 0, sizeof(jpeg), sizeof(jpeg), crc);
    dispatch_chunk(&header, jpeg);
    EXPECT(process_next_event());
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_JPEG, 1201));

    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));
    jpeg[sizeof(jpeg) - 1U] = 0;
    crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    header = make_header(1202, 0, 1, 0, sizeof(jpeg), sizeof(jpeg), crc);
    dispatch_chunk(&header, jpeg);
    EXPECT(process_next_event());
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_JPEG, 1202));
    return true;
}

static bool test_five_second_inactivity_timeout(void)
{
    uint8_t jpeg[15];
    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const sdio_frame_chunk_header_t first =
        make_header(1300, 0, 3, 0, 5, sizeof(jpeg), crc);
    const sdio_frame_chunk_header_t second =
        make_header(1300, 1, 3, 5, 5, sizeof(jpeg), crc);

    dispatch_chunk(&first, jpeg);
    g_now_us += FRAME_ASSEMBLY_TIMEOUT_US - 1;
    check_assembly_timeout();
    EXPECT(g_control_count == 0);
    EXPECT(s_receiver.state == RECEIVER_ASSEMBLING);

    /* A valid chunk refreshes the inactivity deadline. */
    dispatch_chunk(&second, jpeg + 5);
    g_now_us += 2;
    check_assembly_timeout();
    EXPECT(g_control_count == 0);
    EXPECT(s_receiver.state == RECEIVER_ASSEMBLING);

    g_now_us += FRAME_ASSEMBLY_TIMEOUT_US - 2;
    check_assembly_timeout();
    EXPECT(g_control_count == 1);
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_TIMEOUT, 1300));
    EXPECT(g_controls[0].detail == 10);
    EXPECT(s_receiver.state == RECEIVER_IDLE);
    return true;
}

static bool test_ready_not_ready_and_busy_queries(void)
{
    uint8_t jpeg[10];
    reset_fixture();

    g_mesh_ready = false;
    dispatch_query(1400);
    EXPECT(process_next_event());
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_NOT_READY,
                          SDIO_FRAME_REASON_MESH_NOT_READY, 1400));

    g_mesh_ready = true;
    dispatch_query(1401);
    EXPECT(process_next_event());
    EXPECT(expect_control(1, SDIO_FRAME_CONTROL_READY,
                          SDIO_FRAME_REASON_NONE, 1401));
    EXPECT(g_controls[1].detail == SDIO_FRAME_MAX_JPEG_SIZE);

    g_mesh_busy = true;
    dispatch_query(1402);
    EXPECT(process_next_event());
    EXPECT(expect_control(2, SDIO_FRAME_CONTROL_BUSY,
                          SDIO_FRAME_REASON_BUSY, 1402));

    g_mesh_busy = false;
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const sdio_frame_chunk_header_t first =
        make_header(1403, 0, 2, 0, 5, sizeof(jpeg), crc);
    dispatch_chunk(&first, jpeg);

    const sdio_frame_chunk_header_t colliding =
        make_header(1405, 0, 2, 0, 5, sizeof(jpeg), crc);
    dispatch_chunk(&colliding, jpeg);
    EXPECT(process_next_event());
    EXPECT(expect_control(3, SDIO_FRAME_CONTROL_BUSY,
                          SDIO_FRAME_REASON_BUSY, 1405));
    EXPECT(g_controls[3].detail == 1403);

    dispatch_query(1404);
    EXPECT(process_next_event());
    EXPECT(expect_control(4, SDIO_FRAME_CONTROL_BUSY,
                          SDIO_FRAME_REASON_BUSY, 1404));
    EXPECT(g_controls[4].detail == 1403);
    return true;
}

static bool test_new_frame_not_ready_is_terminal(void)
{
    uint8_t jpeg[10];
    reset_fixture();
    g_mesh_ready = false;
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const sdio_frame_chunk_header_t first =
        make_header(1406, 0, 2, 0, 5, sizeof(jpeg), crc);

    dispatch_chunk(&first, jpeg);
    EXPECT(process_next_event());
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_MESH_NOT_READY, 1406));

    dispatch_query(1406);
    EXPECT(process_next_event());
    EXPECT(expect_control(1, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_MESH_NOT_READY, 1406));
    return true;
}

static bool test_stale_ble_completion_is_ignored(void)
{
    static uint8_t jpeg[100];
    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const sdio_frame_chunk_header_t header =
        make_header(1500, 0, 1, 0, sizeof(jpeg), sizeof(jpeg), crc);
    dispatch_chunk(&header, jpeg);
    EXPECT(process_next_event());
    EXPECT(s_receiver.state == RECEIVER_BLE_ACTIVE);
    EXPECT(g_control_count == 1);

    g_mesh_busy = false;
    mesh_frame_done_callback(1501, TEST_BLE_FRAME_ID, ESP_OK, NULL);
    EXPECT(process_next_event());
    EXPECT(s_receiver.state == RECEIVER_BLE_ACTIVE);
    EXPECT(g_control_count == 1);
    return true;
}

static bool test_failed_terminal_is_replayed_after_control_loss(void)
{
    static uint8_t jpeg[100];
    reset_fixture();
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const sdio_frame_chunk_header_t header =
        make_header(1502, 0, 1, 0, sizeof(jpeg), sizeof(jpeg), crc);

    dispatch_chunk(&header, jpeg);
    EXPECT(process_next_event());
    EXPECT(s_receiver.state == RECEIVER_BLE_ACTIVE);
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_ACCEPTED,
                          SDIO_FRAME_REASON_NONE, 1502));

    /* Model a BLE terminal error whose first SDIO FAILED packet is lost at
     * the P4. The C6 is already idle and advertises READY for future frames. */
    g_mesh_busy = false;
    mesh_frame_done_callback(1502, TEST_BLE_FRAME_ID, ESP_ERR_TIMEOUT, NULL);
    EXPECT(process_next_event());
    EXPECT(g_control_count == 3);
    EXPECT(expect_control(1, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_BLE_SEND, 1502));
    EXPECT(g_controls[1].ble_frame_id == TEST_BLE_FRAME_ID);
    EXPECT(g_controls[1].detail == (uint32_t)ESP_ERR_TIMEOUT);
    EXPECT(expect_control(2, SDIO_FRAME_CONTROL_READY,
                          SDIO_FRAME_REASON_NONE, 0));

    /* QUERY for the in-flight ID must replay the exact FAILED result. A
     * matching READY response would make the P4 report a false success. */
    dispatch_query(1502);
    EXPECT(process_next_event());
    EXPECT(g_control_count == 4);
    EXPECT(expect_control(3, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_BLE_SEND, 1502));
    EXPECT(g_controls[3].ble_frame_id == TEST_BLE_FRAME_ID);
    EXPECT(g_controls[3].detail == (uint32_t)ESP_ERR_TIMEOUT);
    return true;
}

static bool test_terminal_survives_full_status_queue(void)
{
    reset_fixture();

    worker_event_t filler = {.kind = WORK_QUERY, .frame_id = 1600};
    while (xQueueSend(s_worker_queue, &filler, 0) == pdTRUE) {
    }
    EXPECT(g_queue.count == g_queue.capacity);

    /* The enqueue itself fails, but the terminal cache is updated first. */
    enqueue_control(SDIO_FRAME_CONTROL_FAILED,
                    1601,
                    TEST_BLE_FRAME_ID,
                    SDIO_FRAME_REASON_BLE_SEND,
                    (uint32_t)ESP_FAIL);
    EXPECT(g_queue.count == g_queue.capacity);
    EXPECT(s_last_terminal.valid);
    EXPECT(s_last_terminal.frame_id == 1601);

    g_queue.head = 0;
    g_queue.count = 0;
    dispatch_query(1601);
    EXPECT(process_next_event());
    EXPECT(g_control_count == 1);
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_FAILED,
                          SDIO_FRAME_REASON_BLE_SEND, 1601));
    EXPECT(g_controls[0].ble_frame_id == TEST_BLE_FRAME_ID);
    EXPECT(g_controls[0].detail == (uint32_t)ESP_FAIL);
    return true;
}

static bool test_time_query_bridge_and_ready_restore(void)
{
    const uint32_t request_id = UINT32_C(0x89abcdef);
    const uint64_t client_tx_us = UINT64_C(123456789);
    const uint64_t server_rx_ms = UINT64_C(1760000123000);
    const uint64_t server_tx_ms = UINT64_C(1760000123002);
    reset_fixture();

    dispatch_time_query(request_id, client_tx_us);
    EXPECT(g_submitted_time_request_id == request_id);
    EXPECT(g_submitted_client_tx_us == client_tx_us);
    EXPECT(g_mesh_busy);
    EXPECT(g_time_sample_count == 0U);

    /* A frame cannot overtake the queued clock operation. */
    uint8_t jpeg[10];
    fill_jpeg(jpeg, sizeof(jpeg));
    const uint32_t crc = esp_crc32_le(0, jpeg, sizeof(jpeg));
    const sdio_frame_chunk_header_t header =
        make_header(1700, 0, 2, 0, 5, sizeof(jpeg), crc);
    dispatch_chunk(&header, jpeg);
    EXPECT(process_next_event());
    EXPECT(expect_control(0, SDIO_FRAME_CONTROL_BUSY,
                          SDIO_FRAME_REASON_BUSY, 1700));

    g_mesh_busy = false;
    mesh_time_done_callback(request_id, client_tx_us, true,
                            server_rx_ms, server_tx_ms, ESP_OK, NULL);
    EXPECT(process_next_event());
    EXPECT(g_time_sample_count == 1U);
    EXPECT(expect_time_sample(0, SDIO_TIME_STATUS_OK,
                              request_id, client_tx_us));
    EXPECT(g_time_samples[0].server_rx_unix_ms == server_rx_ms);
    EXPECT(g_time_samples[0].server_tx_unix_ms == server_tx_ms);
    EXPECT(g_control_count == 2U);
    EXPECT(expect_control(1, SDIO_FRAME_CONTROL_READY,
                          SDIO_FRAME_REASON_NONE, 0U));
    return true;
}

static bool test_time_query_explicit_terminal_statuses(void)
{
    const uint32_t request_id = 1710U;
    const uint64_t client_tx_us = 4567U;

    reset_fixture();
    g_mesh_ready = false;
    dispatch_time_query(request_id, client_tx_us);
    EXPECT(process_next_event());
    EXPECT(expect_time_sample(0, SDIO_TIME_STATUS_NOT_READY,
                              request_id, client_tx_us));

    reset_fixture();
    g_mesh_busy = true;
    dispatch_time_query(request_id + 1U, client_tx_us + 1U);
    EXPECT(process_next_event());
    EXPECT(expect_time_sample(0, SDIO_TIME_STATUS_BUSY,
                              request_id + 1U, client_tx_us + 1U));

    reset_fixture();
    g_time_submit_result = ESP_FAIL;
    dispatch_time_query(request_id + 2U, client_tx_us + 2U);
    EXPECT(process_next_event());
    EXPECT(expect_time_sample(0, SDIO_TIME_STATUS_FAILED,
                              request_id + 2U, client_tx_us + 2U));

    reset_fixture();
    dispatch_time_query(request_id + 3U, client_tx_us + 3U);
    g_mesh_busy = false;
    mesh_time_done_callback(request_id + 3U, client_tx_us + 3U, false,
                            0U, 0U, ESP_OK, NULL);
    EXPECT(process_next_event());
    EXPECT(expect_time_sample(0, SDIO_TIME_STATUS_UNAVAILABLE,
                              request_id + 3U, client_tx_us + 3U));
    EXPECT(g_time_samples[0].server_rx_unix_ms == 0U);
    EXPECT(g_time_samples[0].server_tx_unix_ms == 0U);
    return true;
}

typedef bool (*test_function_t)(void);

typedef struct {
    const char *name;
    test_function_t function;
} test_case_t;

int main(void)
{
    const test_case_t tests[] = {
        {"30720 boundary and BLE lifecycle", test_30720_boundary_and_ble_lifecycle},
        {"30721 rejection", test_30721_is_rejected},
        {"224x224 JPEG opaque forwarding", test_224x224_jpeg_is_forwarded_opaquely},
        {"reverse chunk", test_reverse_chunk_is_rejected},
        {"missing chunk", test_missing_chunk_is_rejected},
        {"duplicate chunk", test_duplicate_chunk_is_rejected},
        {"metadata mismatch", test_metadata_mismatch_is_rejected},
        {"CRC failure", test_crc_error_is_rejected},
        {"JPEG SOI/EOI", test_jpeg_soi_and_eoi_are_required},
        {"5 second inactivity timeout", test_five_second_inactivity_timeout},
        {"READY/NOT_READY/BUSY", test_ready_not_ready_and_busy_queries},
        {"new frame NOT_READY terminal", test_new_frame_not_ready_is_terminal},
        {"stale BLE completion", test_stale_ble_completion_is_ignored},
        {"lost FAILED terminal replay", test_failed_terminal_is_replayed_after_control_loss},
        {"terminal survives full queue", test_terminal_survives_full_status_queue},
        {"time QUERY bridge / READY restore", test_time_query_bridge_and_ready_restore},
        {"time explicit terminal statuses", test_time_query_explicit_terminal_statuses},
    };

    size_t passed = 0;
    for (size_t index = 0; index < ARRAY_SIZE(tests); ++index) {
        const bool success = tests[index].function();
        printf("%-38s %s\n", tests[index].name, success ? "PASS" : "FAIL");
        if (success) {
            passed++;
        }
    }

    printf("receiver tests: %zu/%zu passed\n", passed, ARRAY_SIZE(tests));
    return passed == ARRAY_SIZE(tests) ? EXIT_SUCCESS : EXIT_FAILURE;
}
