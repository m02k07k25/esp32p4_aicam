#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Compile the production BLE image source in this translation unit. Static
 * worker helpers and callbacks are intentionally exercised below; only their
 * ESP-IDF/FreeRTOS boundary is mocked by tests/stubs.
 */
#include "../main/ble_mesh_image_source.c"

#define TEST_PACKET_CAPACITY 16U
#define TEST_PACKET_BYTES    (sizeof(ble_mesh_image_data_header_t) + \
                              BLE_MESH_IMAGE_DATA_BYTES)
#define TEST_JPEG_BYTES      (BLE_MESH_IMAGE_DATA_BYTES * 2U + 1U)
#define TEST_P4_FRAME_ID     UINT32_C(0x12345678)
#define TEST_BLE_FRAME_ID    UINT16_C(0x4321)
#define TEST_NET_IDX         UINT16_C(0x0123)
#define TEST_APP_IDX         UINT16_C(0x0456)
#define TEST_GATEWAY_ADDR    UINT16_C(0x0001)
#define TEST_DETECTED_AT_MS  UINT64_C(1760000123456)
#define TEST_JPEG_CRC32      UINT32_C(0x89abcdef)

typedef struct {
    size_t item_size;
    bool occupied;
    uint8_t item[sizeof(image_job_t)];
} test_queue_t;

typedef struct {
    unsigned int count;
} test_semaphore_t;

typedef enum {
    RESPONSE_SCRIPT_BUSY_NACK = 0,
    RESPONSE_SCRIPT_RESTART,
    RESPONSE_SCRIPT_MID_DATA_RESTART,
    RESPONSE_SCRIPT_OPEN_COMPLETE,
} response_script_t;

typedef struct {
    uint32_t opcode;
    uint16_t length;
    esp_ble_mesh_msg_ctx_t context;
    uint8_t data[TEST_PACKET_BYTES];
} sent_packet_t;

static test_queue_t g_job_queue;
static test_semaphore_t g_send_done_sem;
static test_semaphore_t g_response_sem;
static sent_packet_t g_packets[TEST_PACKET_CAPACITY];
static size_t g_packet_count;
static int64_t g_now_us;
static unsigned int g_response_wait_count;
static unsigned int g_response_give_count;
static bool g_bad_response_wait;
static unsigned int g_frame_done_count;
static uint32_t g_done_p4_frame_id;
static uint16_t g_done_ble_frame_id;
static esp_err_t g_done_status;
static bool g_busy_during_done;
static unsigned int g_restart_count;
static TickType_t g_last_delay_ticks;
static bool g_mesh_provisioned;
static response_script_t g_response_script;

#define EXPECT(expression)                                                       \
    do {                                                                         \
        if (!(expression)) {                                                     \
            fprintf(stderr, "  assertion failed at line %d: %s\n",             \
                    __LINE__, #expression);                                      \
            return false;                                                        \
        }                                                                        \
    } while (0)

static void dispatch_frame_response(uint32_t opcode, uint16_t frame_id)
{
    uint8_t message[sizeof(ble_mesh_image_frame_t)];
    ble_mesh_image_put_le16(message, frame_id);
    esp_ble_mesh_msg_ctx_t context = {
        .net_idx = TEST_NET_IDX,
        .app_idx = TEST_APP_IDX,
        .addr = TEST_GATEWAY_ADDR,
    };
    esp_ble_mesh_model_cb_param_t parameter = {
        .model_operation = {
            .opcode = opcode,
            .model = &s_vendor_models[0],
            .length = sizeof(message),
            .msg = message,
            .ctx = &context,
        },
    };
    custom_model_callback(ESP_BLE_MESH_MODEL_OPERATION_EVT, &parameter);
}

static void dispatch_nack(uint16_t frame_id, uint8_t chunk_index)
{
    uint8_t message[sizeof(ble_mesh_image_nack_header_t) + 1U];
    ble_mesh_image_put_le16(message, frame_id);
    message[2] = 0;
    message[3] = (uint8_t)(1U << chunk_index);
    esp_ble_mesh_msg_ctx_t context = {
        .net_idx = TEST_NET_IDX,
        .app_idx = TEST_APP_IDX,
        .addr = TEST_GATEWAY_ADDR,
    };
    esp_ble_mesh_model_cb_param_t parameter = {
        .model_operation = {
            .opcode = MESH_OPCODE_NACK,
            .model = &s_vendor_models[0],
            .length = sizeof(message),
            .msg = message,
            .ctx = &context,
        },
    };
    custom_model_callback(ESP_BLE_MESH_MODEL_OPERATION_EVT, &parameter);
}

const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ESP_OK" : "host-test-error";
}

void host_test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

int64_t esp_timer_get_time(void)
{
    g_now_us += 1000;
    return g_now_us;
}

void esp_restart(void)
{
    ++g_restart_count;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    if (length != 1U || item_size > sizeof(g_job_queue.item)) {
        return NULL;
    }
    memset(&g_job_queue, 0, sizeof(g_job_queue));
    g_job_queue.item_size = item_size;
    return &g_job_queue;
}

BaseType_t xQueueSend(QueueHandle_t queue,
                      const void *item,
                      TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    test_queue_t *test_queue = queue;
    if (test_queue == NULL || item == NULL || test_queue->occupied ||
        test_queue->item_size == 0U ||
        test_queue->item_size > sizeof(test_queue->item)) {
        return pdFALSE;
    }
    memcpy(test_queue->item, item, test_queue->item_size);
    test_queue->occupied = true;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue,
                         void *item,
                         TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    test_queue_t *test_queue = queue;
    if (test_queue == NULL || item == NULL || !test_queue->occupied) {
        return pdFALSE;
    }
    memcpy(item, test_queue->item, test_queue->item_size);
    test_queue->occupied = false;
    return pdTRUE;
}

BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item)
{
    test_queue_t *test_queue = queue;
    if (test_queue == NULL) {
        return pdFALSE;
    }
    test_queue->occupied = false;
    return xQueueSend(queue, item, 0);
}

void vQueueDelete(QueueHandle_t queue)
{
    (void)queue;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    static test_semaphore_t created_semaphores[3];
    static size_t next_semaphore;

    if (next_semaphore == ARRAY_SIZE(created_semaphores)) {
        return NULL;
    }
    created_semaphores[next_semaphore].count = 0U;
    return &created_semaphores[next_semaphore++];
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    test_semaphore_t *test_semaphore = semaphore;
    if (test_semaphore == NULL) {
        return pdFALSE;
    }
    test_semaphore->count = 1U;
    if (semaphore == &g_response_sem) {
        ++g_response_give_count;
    }
    return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                          TickType_t ticks_to_wait)
{
    test_semaphore_t *test_semaphore = semaphore;
    if (test_semaphore == NULL) {
        return pdFALSE;
    }

    if (semaphore == &g_response_sem && ticks_to_wait != 0U &&
        test_semaphore->count == 0U) {
        ++g_response_wait_count;
        if (ticks_to_wait > pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS)) {
            g_bad_response_wait = true;
        }
        if (g_response_script == RESPONSE_SCRIPT_BUSY_NACK) {
            if (g_response_wait_count == 1U) {
                dispatch_frame_response(MESH_OPCODE_BUSY, s_active_frame_id);
            } else if (g_response_wait_count == 2U) {
                dispatch_frame_response(MESH_OPCODE_ACCEPT, s_active_frame_id);
            } else if (g_response_wait_count == 3U) {
                dispatch_nack(s_active_frame_id, 1U);
                dispatch_nack(s_active_frame_id, 1U);
            } else if (g_response_wait_count == 4U) {
                dispatch_frame_response(MESH_OPCODE_COMPLETE,
                                        s_active_frame_id);
            }
        } else if (g_response_script == RESPONSE_SCRIPT_RESTART) {
            if (g_response_wait_count == 1U ||
                g_response_wait_count == 3U) {
                dispatch_frame_response(MESH_OPCODE_ACCEPT, s_active_frame_id);
            } else if (g_response_wait_count == 2U) {
                dispatch_frame_response(MESH_OPCODE_RESTART,
                                        s_active_frame_id);
            } else if (g_response_wait_count == 4U) {
                dispatch_frame_response(MESH_OPCODE_COMPLETE,
                                        s_active_frame_id);
            }
        } else if (g_response_script == RESPONSE_SCRIPT_MID_DATA_RESTART) {
            if (g_response_wait_count == 1U ||
                g_response_wait_count == 2U) {
                dispatch_frame_response(MESH_OPCODE_ACCEPT, s_active_frame_id);
            } else if (g_response_wait_count == 3U) {
                dispatch_frame_response(MESH_OPCODE_COMPLETE,
                                        s_active_frame_id);
            }
        } else if (g_response_script == RESPONSE_SCRIPT_OPEN_COMPLETE &&
                   g_response_wait_count == 1U) {
            dispatch_frame_response(MESH_OPCODE_COMPLETE, s_active_frame_id);
        }
    }

    if (test_semaphore->count == 0U) {
        return pdFALSE;
    }
    --test_semaphore->count;
    return pdTRUE;
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
        *created_task = (TaskHandle_t)(uintptr_t)1U;
    }
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

void vTaskDelay(TickType_t ticks_to_delay)
{
    g_last_delay_ticks = ticks_to_delay;
    g_now_us += (int64_t)ticks_to_delay * 1000;
}

uint32_t esp_random(void)
{
    return 500U;
}

esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}

esp_err_t nvs_open(const char *namespace_name,
                   int open_mode,
                   nvs_handle_t *out_handle)
{
    (void)namespace_name;
    (void)open_mode;
    (void)out_handle;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    (void)key;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

esp_err_t esp_ble_mesh_register_prov_callback(
    void (*callback)(esp_ble_mesh_prov_cb_event_t,
                     esp_ble_mesh_prov_cb_param_t *))
{
    (void)callback;
    return ESP_OK;
}

esp_err_t esp_ble_mesh_register_config_server_callback(
    void (*callback)(esp_ble_mesh_cfg_server_cb_event_t,
                     esp_ble_mesh_cfg_server_cb_param_t *))
{
    (void)callback;
    return ESP_OK;
}

esp_err_t esp_ble_mesh_register_custom_model_callback(
    void (*callback)(esp_ble_mesh_model_cb_event_t,
                     esp_ble_mesh_model_cb_param_t *))
{
    (void)callback;
    return ESP_OK;
}

esp_err_t esp_ble_mesh_init(esp_ble_mesh_prov_t *provision,
                            esp_ble_mesh_comp_t *composition)
{
    (void)provision;
    (void)composition;
    return ESP_OK;
}

esp_err_t esp_ble_mesh_node_prov_enable(esp_ble_mesh_prov_bearer_t bearers)
{
    (void)bearers;
    return ESP_OK;
}

bool esp_ble_mesh_node_is_provisioned(void)
{
    return g_mesh_provisioned;
}

const uint8_t *esp_ble_mesh_node_get_local_net_key(uint16_t net_idx)
{
    (void)net_idx;
    return NULL;
}

const uint8_t *esp_ble_mesh_node_get_local_app_key(uint16_t app_idx)
{
    (void)app_idx;
    return NULL;
}

esp_err_t esp_ble_mesh_server_model_send_msg(esp_ble_mesh_model_t *model,
                                              esp_ble_mesh_msg_ctx_t *context,
                                              uint32_t opcode,
                                              uint16_t length,
                                              uint8_t *data)
{
    if (model != &s_vendor_models[0] || context == NULL || data == NULL ||
        length > TEST_PACKET_BYTES || g_packet_count == TEST_PACKET_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }

    sent_packet_t *packet = &g_packets[g_packet_count++];
    packet->opcode = opcode;
    packet->length = length;
    packet->context = *context;
    memcpy(packet->data, data, length);

    if (g_response_script == RESPONSE_SCRIPT_MID_DATA_RESTART &&
        opcode == MESH_OPCODE_DATA && g_packet_count == 2U) {
        dispatch_frame_response(MESH_OPCODE_RESTART, s_active_frame_id);
    }

    esp_ble_mesh_model_cb_param_t completion = {
        .model_send_comp = {
            .model = model,
            .opcode = opcode,
            .err_code = ESP_OK,
        },
    };
    custom_model_callback(ESP_BLE_MESH_MODEL_SEND_COMP_EVT, &completion);
    return ESP_OK;
}

static void frame_done_callback(uint32_t p4_frame_id,
                                uint16_t ble_frame_id,
                                esp_err_t status,
                                void *user_ctx)
{
    (void)user_ctx;
    ++g_frame_done_count;
    g_done_p4_frame_id = p4_frame_id;
    g_done_ble_frame_id = ble_frame_id;
    g_done_status = status;
    g_busy_during_done = ble_mesh_image_source_is_busy();
}

static void reset_fixture(void)
{
    memset(&g_job_queue, 0, sizeof(g_job_queue));
    g_job_queue.item_size = sizeof(image_job_t);
    memset(&g_send_done_sem, 0, sizeof(g_send_done_sem));
    memset(&g_response_sem, 0, sizeof(g_response_sem));
    memset(g_packets, 0, sizeof(g_packets));
    g_packet_count = 0U;
    g_now_us = 0;
    g_response_wait_count = 0U;
    g_response_give_count = 0U;
    g_bad_response_wait = false;
    g_frame_done_count = 0U;
    g_done_p4_frame_id = 0U;
    g_done_ble_frame_id = 0U;
    g_done_status = ESP_FAIL;
    g_busy_during_done = true;
    g_restart_count = 0U;
    g_last_delay_ticks = 0U;
    g_mesh_provisioned = true;
    g_response_script = RESPONSE_SCRIPT_BUSY_NACK;

    s_job_queue = &g_job_queue;
    s_send_done_sem = &g_send_done_sem;
    s_response_sem = &g_response_sem;
    s_worker_task = NULL;
    s_initializing = false;
    s_initialized = true;
    s_init_failed = false;
    s_transport_healthy = true;
    s_transport_restart_pending = false;
    s_mesh_bound = true;
    s_ready = true;
    s_outstanding = false;
    s_net_idx = TEST_NET_IDX;
    s_app_idx = TEST_APP_IDX;
    s_binding_generation = 9U;
    s_next_frame_id = TEST_BLE_FRAME_ID;
    memset(&s_callbacks, 0, sizeof(s_callbacks));
    s_callback_ctx = NULL;
    s_send_waiting = false;
    s_send_completed = false;
    s_send_result = ESP_FAIL;
    s_send_opcode = 0U;
    s_active = false;
    s_active_frame_id = 0U;
    s_active_total_chunks = 0U;
    s_active_net_idx = ESP_BLE_MESH_KEY_UNUSED;
    s_active_app_idx = ESP_BLE_MESH_KEY_UNUSED;
    s_active_destination = 0U;
    s_active_binding_generation = 0U;
    s_gateway_response = RESPONSE_NONE;
    s_reject_reason = 0U;
    memset(s_nack_bitmap, 0, sizeof(s_nack_bitmap));
    s_image_publication.publish_addr = TEST_GATEWAY_ADDR;
    s_image_publication.app_idx = TEST_APP_IDX;
    s_image_publication.ttl = 3U;
}

static bool packet_has_route(const sent_packet_t *sent)
{
    EXPECT(sent->context.net_idx == TEST_NET_IDX);
    EXPECT(sent->context.app_idx == TEST_APP_IDX);
    EXPECT(sent->context.addr == TEST_GATEWAY_ADDR);
    EXPECT(sent->context.send_ttl == 3U);
    return true;
}

static bool packet_is_open(size_t packet_index)
{
    EXPECT(packet_index < g_packet_count);
    const sent_packet_t *sent = &g_packets[packet_index];
    EXPECT(sent->opcode == MESH_OPCODE_OPEN);
    EXPECT(sent->length == sizeof(ble_mesh_image_open_t));
    EXPECT(packet_has_route(sent));
    EXPECT(ble_mesh_image_get_le16(sent->data) == TEST_BLE_FRAME_ID);
    EXPECT(ble_mesh_image_get_le64(sent->data + 2) == TEST_DETECTED_AT_MS);
    EXPECT(ble_mesh_image_get_le16(sent->data + 10) == TEST_JPEG_BYTES);
    EXPECT(ble_mesh_image_get_le32(sent->data + 12) == TEST_JPEG_CRC32);
    return true;
}

static bool packet_is_chunk(size_t packet_index,
                            uint8_t chunk_index,
                            const uint8_t *jpeg,
                            uint16_t expected_data_len)
{
    EXPECT(packet_index < g_packet_count);
    const sent_packet_t *sent = &g_packets[packet_index];
    EXPECT(sent->opcode == MESH_OPCODE_DATA);
    EXPECT(sent->length == sizeof(ble_mesh_image_data_header_t) +
                            expected_data_len);
    EXPECT(packet_has_route(sent));
    EXPECT(ble_mesh_image_get_le16(sent->data) == TEST_BLE_FRAME_ID);
    EXPECT(sent->data[2] == chunk_index);
    EXPECT(memcmp(sent->data + sizeof(ble_mesh_image_data_header_t),
                  jpeg + (size_t)chunk_index * BLE_MESH_IMAGE_DATA_BYTES,
                  expected_data_len) == 0);
    return true;
}

static bool packet_is_end(size_t packet_index)
{
    EXPECT(packet_index < g_packet_count);
    const sent_packet_t *sent = &g_packets[packet_index];
    EXPECT(sent->opcode == MESH_OPCODE_END);
    EXPECT(sent->length == sizeof(ble_mesh_image_frame_t));
    EXPECT(packet_has_route(sent));
    EXPECT(ble_mesh_image_get_le16(sent->data) == TEST_BLE_FRAME_ID);
    return true;
}

static bool test_production_mesh_transfer(void)
{
    reset_fixture();

    ble_mesh_image_source_callbacks_t callbacks = {
        .frame_done = frame_done_callback,
    };
    EXPECT(ble_mesh_image_source_register_callbacks(&callbacks, NULL) == ESP_OK);

    uint8_t jpeg[TEST_JPEG_BYTES];
    for (size_t i = 0; i < sizeof(jpeg); ++i) {
        jpeg[i] = (uint8_t)((i * 29U + 7U) & 0xffU);
    }

    uint16_t assigned_frame_id = 0U;
    EXPECT(ble_mesh_image_source_submit(jpeg, sizeof(jpeg), TEST_P4_FRAME_ID,
                                        TEST_DETECTED_AT_MS, TEST_JPEG_CRC32,
                                        &assigned_frame_id) == ESP_OK);
    EXPECT(assigned_frame_id == TEST_BLE_FRAME_ID);
    EXPECT(ble_mesh_image_source_is_busy());

    image_job_t queued_job;
    EXPECT(xQueueReceive(s_job_queue, &queued_job, 0) == pdTRUE);
    EXPECT(queued_job.jpeg == jpeg);
    EXPECT(queued_job.len == sizeof(jpeg));
    EXPECT(queued_job.p4_frame_id == TEST_P4_FRAME_ID);
    EXPECT(queued_job.detected_at_ms == TEST_DETECTED_AT_MS);
    EXPECT(queued_job.jpeg_crc32 == TEST_JPEG_CRC32);
    EXPECT(queued_job.ble_frame_id == TEST_BLE_FRAME_ID);

    EXPECT(send_image(&queued_job) == ESP_OK);
    EXPECT(!s_active);
    EXPECT(ble_mesh_image_source_is_busy());
    EXPECT(g_packet_count == 8U);

    EXPECT(packet_is_open(0U));
    EXPECT(packet_is_open(1U));
    EXPECT(packet_is_chunk(2U, 0U, jpeg, BLE_MESH_IMAGE_DATA_BYTES));
    EXPECT(packet_is_chunk(3U, 1U, jpeg, BLE_MESH_IMAGE_DATA_BYTES));
    EXPECT(packet_is_chunk(4U, 2U, jpeg, 1U));
    EXPECT(packet_is_end(5U));
    EXPECT(packet_is_chunk(6U, 1U, jpeg, BLE_MESH_IMAGE_DATA_BYTES));
    EXPECT(packet_is_end(7U));

    EXPECT(g_response_wait_count == 4U);
    EXPECT(g_response_give_count == 5U);
    EXPECT(!g_bad_response_wait);
    EXPECT(g_last_delay_ticks == pdMS_TO_TICKS(500U));

    publish_frame_done(&queued_job, ESP_OK);
    EXPECT(g_frame_done_count == 1U);
    EXPECT(g_done_p4_frame_id == TEST_P4_FRAME_ID);
    EXPECT(g_done_ble_frame_id == TEST_BLE_FRAME_ID);
    EXPECT(g_done_status == ESP_OK);
    EXPECT(!g_busy_during_done);
    EXPECT(!ble_mesh_image_source_is_busy());
    EXPECT(g_restart_count == 0U);
    return true;
}

static bool test_transport_timeout_recovery_boundary(void)
{
    reset_fixture();
    EXPECT(ble_mesh_image_source_is_ready());

    mark_transport_fault();
    EXPECT(!ble_mesh_image_source_is_ready());
    EXPECT(s_transport_restart_pending);

    restart_after_transport_fault();
    EXPECT(g_last_delay_ticks == pdMS_TO_TICKS(TRANSPORT_RESTART_DELAY_MS));
    EXPECT(g_restart_count == 1U);
    return true;
}

static bool test_gateway_restart_resends_full_frame(void)
{
    reset_fixture();
    g_response_script = RESPONSE_SCRIPT_RESTART;

    uint8_t jpeg[TEST_JPEG_BYTES];
    for (size_t i = 0; i < sizeof(jpeg); ++i) {
        jpeg[i] = (uint8_t)(i ^ 0xa5U);
    }
    const image_job_t job = {
        .jpeg = jpeg,
        .len = sizeof(jpeg),
        .p4_frame_id = TEST_P4_FRAME_ID,
        .detected_at_ms = TEST_DETECTED_AT_MS,
        .jpeg_crc32 = TEST_JPEG_CRC32,
        .ble_frame_id = TEST_BLE_FRAME_ID,
    };

    EXPECT(send_image(&job) == ESP_OK);
    EXPECT(g_response_wait_count == 4U);
    EXPECT(g_packet_count == 10U);
    EXPECT(packet_is_open(0U));
    EXPECT(packet_is_chunk(1U, 0U, jpeg, BLE_MESH_IMAGE_DATA_BYTES));
    EXPECT(packet_is_chunk(2U, 1U, jpeg, BLE_MESH_IMAGE_DATA_BYTES));
    EXPECT(packet_is_chunk(3U, 2U, jpeg, 1U));
    EXPECT(packet_is_end(4U));
    EXPECT(packet_is_open(5U));
    EXPECT(packet_is_chunk(6U, 0U, jpeg, BLE_MESH_IMAGE_DATA_BYTES));
    EXPECT(packet_is_chunk(7U, 1U, jpeg, BLE_MESH_IMAGE_DATA_BYTES));
    EXPECT(packet_is_chunk(8U, 2U, jpeg, 1U));
    EXPECT(packet_is_end(9U));
    return true;
}

static bool test_duplicate_open_complete_short_circuits(void)
{
    reset_fixture();
    g_response_script = RESPONSE_SCRIPT_OPEN_COMPLETE;

    uint8_t jpeg[TEST_JPEG_BYTES] = {0};
    const image_job_t job = {
        .jpeg = jpeg,
        .len = sizeof(jpeg),
        .p4_frame_id = TEST_P4_FRAME_ID,
        .detected_at_ms = TEST_DETECTED_AT_MS,
        .jpeg_crc32 = TEST_JPEG_CRC32,
        .ble_frame_id = TEST_BLE_FRAME_ID,
    };

    EXPECT(send_image(&job) == ESP_OK);
    EXPECT(g_response_wait_count == 1U);
    EXPECT(g_packet_count == 1U);
    EXPECT(packet_is_open(0U));
    return true;
}

static bool test_mid_data_restart_stops_wasted_chunks(void)
{
    reset_fixture();
    g_response_script = RESPONSE_SCRIPT_MID_DATA_RESTART;

    uint8_t jpeg[TEST_JPEG_BYTES];
    for (size_t i = 0; i < sizeof(jpeg); ++i) {
        jpeg[i] = (uint8_t)(i + 3U);
    }
    const image_job_t job = {
        .jpeg = jpeg,
        .len = sizeof(jpeg),
        .p4_frame_id = TEST_P4_FRAME_ID,
        .detected_at_ms = TEST_DETECTED_AT_MS,
        .jpeg_crc32 = TEST_JPEG_CRC32,
        .ble_frame_id = TEST_BLE_FRAME_ID,
    };

    EXPECT(send_image(&job) == ESP_OK);
    EXPECT(g_response_wait_count == 3U);
    EXPECT(g_packet_count == 7U);
    EXPECT(packet_is_open(0U));
    EXPECT(packet_is_chunk(1U, 0U, jpeg, BLE_MESH_IMAGE_DATA_BYTES));
    EXPECT(packet_is_open(2U));
    EXPECT(packet_is_chunk(3U, 0U, jpeg, BLE_MESH_IMAGE_DATA_BYTES));
    EXPECT(packet_is_chunk(4U, 1U, jpeg, BLE_MESH_IMAGE_DATA_BYTES));
    EXPECT(packet_is_chunk(5U, 2U, jpeg, 1U));
    EXPECT(packet_is_end(6U));
    return true;
}

static bool test_publication_is_required_for_ready(void)
{
    reset_fixture();

    s_image_publication.publish_addr = 0U;
    refresh_ready_state();
    EXPECT(!ble_mesh_image_source_is_ready());

    s_image_publication.publish_addr = TEST_GATEWAY_ADDR;
    s_image_publication.app_idx = TEST_APP_IDX + 1U;
    refresh_ready_state();
    EXPECT(!ble_mesh_image_source_is_ready());

    s_image_publication.app_idx = TEST_APP_IDX;
    refresh_ready_state();
    EXPECT(ble_mesh_image_source_is_ready());

    g_mesh_provisioned = false;
    refresh_ready_state();
    EXPECT(!ble_mesh_image_source_is_ready());
    return true;
}

int main(void)
{
    printf("production BLE Mesh source host tests\n");
    if (!test_production_mesh_transfer()) {
        printf("  FAIL mesh transfer / NACK repair / zero-copy lifecycle\n");
        return 1;
    }
    printf("  PASS mesh transfer / NACK repair / zero-copy lifecycle\n");
    if (!test_transport_timeout_recovery_boundary()) {
        printf("  FAIL BLE timeout recovery restart boundary\n");
        return 1;
    }
    printf("  PASS BLE timeout recovery restart boundary\n");
    if (!test_gateway_restart_resends_full_frame()) {
        printf("  FAIL Gateway RESTART full resend\n");
        return 1;
    }
    printf("  PASS Gateway RESTART full resend\n");
    if (!test_duplicate_open_complete_short_circuits()) {
        printf("  FAIL duplicate OPEN idempotent COMPLETE\n");
        return 1;
    }
    printf("  PASS duplicate OPEN idempotent COMPLETE\n");
    if (!test_mid_data_restart_stops_wasted_chunks()) {
        printf("  FAIL early DATA-phase RESTART\n");
        return 1;
    }
    printf("  PASS early DATA-phase RESTART\n");
    if (!test_publication_is_required_for_ready()) {
        printf("  FAIL provision/bind/publication READY gate\n");
        return 1;
    }
    printf("  PASS provision/bind/publication READY gate\n");
    return 0;
}
