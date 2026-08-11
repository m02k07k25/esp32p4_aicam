#include "ble_mesh_image_source.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"

#if defined(CONFIG_BT_NIMBLE_ENABLED)
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#elif defined(CONFIG_BT_BLUEDROID_ENABLED)
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#endif

#include "ble_mesh_image_protocol.h"

#define TAG "ble_mesh_img"

#define MESH_OPCODE(raw_opcode) \
    ESP_BLE_MESH_MODEL_OP_3((raw_opcode), BLE_MESH_IMAGE_COMPANY_ID)
#define MESH_OPCODE_OPEN       MESH_OPCODE(BLE_MESH_IMAGE_OP_OPEN)
#define MESH_OPCODE_DATA       MESH_OPCODE(BLE_MESH_IMAGE_OP_DATA)
#define MESH_OPCODE_END        MESH_OPCODE(BLE_MESH_IMAGE_OP_END)
#define MESH_OPCODE_ACCEPT     MESH_OPCODE(BLE_MESH_IMAGE_OP_ACCEPT)
#define MESH_OPCODE_BUSY       MESH_OPCODE(BLE_MESH_IMAGE_OP_BUSY)
#define MESH_OPCODE_COMPLETE   MESH_OPCODE(BLE_MESH_IMAGE_OP_COMPLETE)
#define MESH_OPCODE_NACK       MESH_OPCODE(BLE_MESH_IMAGE_OP_NACK)
#define MESH_OPCODE_RESTART    MESH_OPCODE(BLE_MESH_IMAGE_OP_RESTART)
#define MESH_OPCODE_REJECT     MESH_OPCODE(BLE_MESH_IMAGE_OP_REJECT)

#define IMAGE_WORKER_STACK_BYTES       6144U
#define IMAGE_WORKER_PRIORITY          5U
#define LOCAL_SEND_TIMEOUT_MS          10000U
#define LOCAL_SEND_RETRIES             5U
#define ENQUEUE_RETRY_DELAY_MS         100U
#define BUSY_RETRY_DELAY_MS            250U
#define RESPONSE_TIMEOUT_MS            5000U
#define OPEN_ATTEMPTS                  5U
#define REPAIR_ROUNDS                  5U
#define SESSION_RESTARTS               5U
#define FRAME_DEADLINE_US              (300LL * 1000LL * 1000LL)
#define BT_SYNC_TIMEOUT_MS             10000U
#define TRANSPORT_RESTART_DELAY_MS     2000U

#define BINDING_NVS_NAMESPACE          "ble_img_src"
#define BINDING_NVS_NET_KEY            "net_idx"
#define BINDING_NVS_APP_KEY            "app_idx"
#define KEY_INDEX_MAX                  0x0FFFU
#define APP_NET_CACHE_ENTRIES          16U
#define NACK_BITMAP_BYTES \
    ((BLE_MESH_IMAGE_MAX_CHUNKS + 7U) / 8U)

typedef struct {
    const uint8_t *jpeg;
    size_t len;
    uint32_t p4_frame_id;
    uint64_t detected_at_ms;
    uint32_t jpeg_crc32;
    uint16_t ble_frame_id;
} image_job_t;

typedef enum {
    RESPONSE_NONE = 0,
    RESPONSE_ACCEPT,
    RESPONSE_BUSY,
    RESPONSE_COMPLETE,
    RESPONSE_NACK,
    RESPONSE_RESTART,
    RESPONSE_REJECT,
} gateway_response_t;

typedef struct {
    uint16_t net_idx;
    uint16_t app_idx;
    uint16_t destination;
    uint8_t ttl;
    uint32_t binding_generation;
} mesh_route_t;

typedef enum {
    SESSION_RESULT_FAILED = 0,
    SESSION_RESULT_ACCEPTED,
    SESSION_RESULT_COMPLETE,
    SESSION_RESULT_RESTART,
} session_result_t;

typedef struct {
    bool valid;
    uint16_t app_idx;
    uint16_t net_idx;
} app_net_entry_t;

static uint8_t s_dev_uuid[ESP_BLE_MESH_OCTET16_LEN] = {0x32, 0x10};

static esp_ble_mesh_cfg_srv_t s_config_server = {
    /* Segmented unicast already repairs missing Lower Transport segments. */
    .net_transmit = ESP_BLE_MESH_TRANSMIT(0, 20),
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(0, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
#if defined(CONFIG_BLE_MESH_GATT_PROXY_SERVER)
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
#else
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
#endif
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
    .default_ttl = 3,
};

static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_config_server),
};

static esp_ble_mesh_model_op_t s_vendor_ops[] = {
    ESP_BLE_MESH_MODEL_OP(MESH_OPCODE_ACCEPT,
                          sizeof(ble_mesh_image_frame_t)),
    ESP_BLE_MESH_MODEL_OP(MESH_OPCODE_BUSY,
                          sizeof(ble_mesh_image_frame_t)),
    ESP_BLE_MESH_MODEL_OP(MESH_OPCODE_COMPLETE,
                          sizeof(ble_mesh_image_frame_t)),
    ESP_BLE_MESH_MODEL_OP(MESH_OPCODE_NACK,
                          sizeof(ble_mesh_image_nack_header_t) + 1U),
    ESP_BLE_MESH_MODEL_OP(MESH_OPCODE_RESTART,
                          sizeof(ble_mesh_image_frame_t)),
    ESP_BLE_MESH_MODEL_OP(MESH_OPCODE_REJECT,
                          sizeof(ble_mesh_image_reject_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

/* The source sends explicitly, but a publication context lets the Provisioner
 * configure and persist the Gateway destination/AppKey/TTL. */
ESP_BLE_MESH_MODEL_PUB_DEFINE(s_image_publication, 1, ROLE_NODE);

static esp_ble_mesh_model_t s_vendor_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(BLE_MESH_IMAGE_COMPANY_ID,
                              BLE_MESH_IMAGE_SOURCE_MODEL_ID,
                              s_vendor_ops, &s_image_publication, NULL),
};

static esp_ble_mesh_elem_t s_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, s_root_models, s_vendor_models),
};

static esp_ble_mesh_comp_t s_composition = {
    .cid = BLE_MESH_IMAGE_COMPANY_ID,
    .element_count = ARRAY_SIZE(s_elements),
    .elements = s_elements,
};

static esp_ble_mesh_prov_t s_provision = {
    .uuid = s_dev_uuid,
};

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_job_queue;
static SemaphoreHandle_t s_send_done_sem;
static SemaphoreHandle_t s_response_sem;
static TaskHandle_t s_worker_task;

static bool s_initializing;
static bool s_initialized;
static bool s_init_failed;
static bool s_transport_healthy = true;
static bool s_transport_restart_pending;
static bool s_mesh_bound;
static bool s_ready;
static bool s_outstanding;
static uint16_t s_net_idx = ESP_BLE_MESH_KEY_UNUSED;
static uint16_t s_app_idx = ESP_BLE_MESH_KEY_UNUSED;
static uint32_t s_binding_generation;
static uint16_t s_next_frame_id = 1U;

static ble_mesh_image_source_callbacks_t s_callbacks;
static void *s_callback_ctx;

static bool s_send_waiting;
static bool s_send_completed;
static esp_err_t s_send_result = ESP_FAIL;
static uint32_t s_send_opcode;

static bool s_active;
static uint16_t s_active_frame_id;
static uint16_t s_active_total_chunks;
static uint16_t s_active_net_idx = ESP_BLE_MESH_KEY_UNUSED;
static uint16_t s_active_app_idx = ESP_BLE_MESH_KEY_UNUSED;
static uint16_t s_active_destination;
static uint32_t s_active_binding_generation;
static uint8_t s_nack_bitmap[NACK_BITMAP_BYTES];
static gateway_response_t s_gateway_response;
static uint8_t s_reject_reason;

static app_net_entry_t s_app_net_cache[APP_NET_CACHE_ENTRIES];

#if defined(CONFIG_BT_NIMBLE_ENABLED)
static SemaphoreHandle_t s_bt_sync_sem;
static uint8_t s_bt_addr_type;
static uint8_t s_bt_addr[6];
static esp_err_t s_bt_sync_result = ESP_FAIL;
void ble_store_config_init(void);
#endif

static void image_worker(void *arg);
static void refresh_ready_state(void);

void __attribute__((weak)) ble_mesh_image_source_ready_changed(bool ready)
{
    (void)ready;
}

void __attribute__((weak)) ble_mesh_image_source_frame_done(
    uint32_t p4_frame_id, uint16_t ble_frame_id, esp_err_t status)
{
    (void)p4_frame_id;
    (void)ble_frame_id;
    (void)status;
}

static void publish_ready_change(bool ready)
{
    ble_mesh_image_source_ready_changed_cb_t callback;
    void *callback_ctx;

    portENTER_CRITICAL(&s_state_lock);
    callback = s_callbacks.ready_changed;
    callback_ctx = s_callback_ctx;
    portEXIT_CRITICAL(&s_state_lock);

    ble_mesh_image_source_ready_changed(ready);
    if (callback != NULL) {
        callback(ready, callback_ctx);
    }
}

static void set_ready_state(bool ready)
{
    bool changed = false;

    portENTER_CRITICAL(&s_state_lock);
    if (s_ready != ready) {
        s_ready = ready;
        changed = true;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (changed) {
        ESP_LOGI(TAG, "BLE image source is %s", ready ? "ready" : "not ready");
        publish_ready_change(ready);
    }
}

static void refresh_ready_state(void)
{
    bool ready;
    const bool provisioned = esp_ble_mesh_node_is_provisioned();

    portENTER_CRITICAL(&s_state_lock);
    ready = s_initialized && provisioned && s_mesh_bound &&
            s_transport_healthy &&
            s_image_publication.publish_addr >= 0x0001U &&
            s_image_publication.publish_addr <= 0x7fffU &&
            s_image_publication.app_idx == s_app_idx;
    portEXIT_CRITICAL(&s_state_lock);

    set_ready_state(ready);
}

static void publish_frame_done(const image_job_t *job, esp_err_t status)
{
    ble_mesh_image_source_frame_done_cb_t callback;
    void *callback_ctx;

    portENTER_CRITICAL(&s_state_lock);
    s_outstanding = false;
    callback = s_callbacks.frame_done;
    callback_ctx = s_callback_ctx;
    portEXIT_CRITICAL(&s_state_lock);

    /* No module code reads job->jpeg after this point. */
    ble_mesh_image_source_frame_done(job->p4_frame_id,
                                     job->ble_frame_id,
                                     status);
    if (callback != NULL) {
        callback(job->p4_frame_id, job->ble_frame_id, status, callback_ctx);
    }
}

esp_err_t ble_mesh_image_source_register_callbacks(
    const ble_mesh_image_source_callbacks_t *callbacks,
    void *user_ctx)
{
    portENTER_CRITICAL(&s_state_lock);
    if (callbacks != NULL) {
        s_callbacks = *callbacks;
        s_callback_ctx = user_ctx;
    } else {
        memset(&s_callbacks, 0, sizeof(s_callbacks));
        s_callback_ctx = NULL;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

bool ble_mesh_image_source_is_ready(void)
{
    bool ready;

    portENTER_CRITICAL(&s_state_lock);
    ready = s_ready;
    portEXIT_CRITICAL(&s_state_lock);
    return ready;
}

bool ble_mesh_image_source_is_busy(void)
{
    bool busy;

    portENTER_CRITICAL(&s_state_lock);
    busy = s_outstanding;
    portEXIT_CRITICAL(&s_state_lock);
    return busy;
}

static esp_err_t binding_storage_save(uint16_t net_idx, uint16_t app_idx)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BINDING_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u16(handle, BINDING_NVS_NET_KEY, net_idx);
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, BINDING_NVS_APP_KEY, app_idx);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t binding_storage_load(uint16_t *net_idx, uint16_t *app_idx)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (net_idx == NULL || app_idx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(BINDING_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u16(handle, BINDING_NVS_NET_KEY, net_idx);
    if (err == ESP_OK) {
        err = nvs_get_u16(handle, BINDING_NVS_APP_KEY, app_idx);
    }
    nvs_close(handle);
    return err;
}

static void binding_storage_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BINDING_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return;
    }

    err = nvs_erase_key(handle, BINDING_NVS_NET_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_erase_key(handle, BINDING_NVS_APP_KEY);
    }
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

static bool model_has_app_idx(uint16_t app_idx)
{
    for (size_t i = 0; i < ARRAY_SIZE(s_vendor_models[0].keys); ++i) {
        if (s_vendor_models[0].keys[i] == app_idx) {
            return true;
        }
    }
    return false;
}

static uint16_t first_bound_app_idx(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(s_vendor_models[0].keys); ++i) {
        if (s_vendor_models[0].keys[i] != ESP_BLE_MESH_KEY_UNUSED) {
            return s_vendor_models[0].keys[i];
        }
    }
    return ESP_BLE_MESH_KEY_UNUSED;
}

static uint16_t unique_local_net_idx(void)
{
    uint16_t found = ESP_BLE_MESH_KEY_UNUSED;

    for (uint16_t idx = 0; idx <= KEY_INDEX_MAX; ++idx) {
        if (esp_ble_mesh_node_get_local_net_key(idx) == NULL) {
            continue;
        }
        if (found != ESP_BLE_MESH_KEY_UNUSED) {
            return ESP_BLE_MESH_KEY_UNUSED;
        }
        found = idx;
    }
    return found;
}

static void app_net_cache_put(uint16_t app_idx, uint16_t net_idx)
{
    size_t free_slot = APP_NET_CACHE_ENTRIES;

    portENTER_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < APP_NET_CACHE_ENTRIES; ++i) {
        if (s_app_net_cache[i].valid &&
            s_app_net_cache[i].app_idx == app_idx) {
            s_app_net_cache[i].net_idx = net_idx;
            portEXIT_CRITICAL(&s_state_lock);
            return;
        }
        if (!s_app_net_cache[i].valid && free_slot == APP_NET_CACHE_ENTRIES) {
            free_slot = i;
        }
    }
    if (free_slot < APP_NET_CACHE_ENTRIES) {
        s_app_net_cache[free_slot].valid = true;
        s_app_net_cache[free_slot].app_idx = app_idx;
        s_app_net_cache[free_slot].net_idx = net_idx;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void app_net_cache_remove(uint16_t app_idx)
{
    portENTER_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < APP_NET_CACHE_ENTRIES; ++i) {
        if (s_app_net_cache[i].valid &&
            s_app_net_cache[i].app_idx == app_idx) {
            s_app_net_cache[i].valid = false;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static uint16_t app_net_cache_get(uint16_t app_idx)
{
    uint16_t net_idx = ESP_BLE_MESH_KEY_UNUSED;

    portENTER_CRITICAL(&s_state_lock);
    for (size_t i = 0; i < APP_NET_CACHE_ENTRIES; ++i) {
        if (s_app_net_cache[i].valid &&
            s_app_net_cache[i].app_idx == app_idx) {
            net_idx = s_app_net_cache[i].net_idx;
            break;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    return net_idx;
}

static void set_runtime_binding(uint16_t net_idx, uint16_t app_idx, bool bound)
{
    portENTER_CRITICAL(&s_state_lock);
    ++s_binding_generation;
    s_mesh_bound = bound;
    s_net_idx = bound ? net_idx : ESP_BLE_MESH_KEY_UNUSED;
    s_app_idx = bound ? app_idx : ESP_BLE_MESH_KEY_UNUSED;
    portEXIT_CRITICAL(&s_state_lock);
    refresh_ready_state();
}

static bool binding_is_locally_valid(uint16_t net_idx, uint16_t app_idx)
{
    return net_idx <= KEY_INDEX_MAX && app_idx <= KEY_INDEX_MAX &&
           model_has_app_idx(app_idx) &&
           esp_ble_mesh_node_get_local_net_key(net_idx) != NULL &&
           esp_ble_mesh_node_get_local_app_key(app_idx) != NULL;
}

static void restore_binding_from_mesh(void)
{
    uint16_t net_idx = ESP_BLE_MESH_KEY_UNUSED;
    uint16_t app_idx = ESP_BLE_MESH_KEY_UNUSED;

    if (!esp_ble_mesh_node_is_provisioned()) {
        set_runtime_binding(ESP_BLE_MESH_KEY_UNUSED,
                            ESP_BLE_MESH_KEY_UNUSED, false);
        return;
    }

    if (binding_storage_load(&net_idx, &app_idx) == ESP_OK &&
        binding_is_locally_valid(net_idx, app_idx)) {
        set_runtime_binding(net_idx, app_idx, true);
        ESP_LOGI(TAG, "restored binding net=0x%03x app=0x%03x",
                 net_idx, app_idx);
        return;
    }

    app_idx = first_bound_app_idx();
    net_idx = unique_local_net_idx();
    if (app_idx != ESP_BLE_MESH_KEY_UNUSED &&
        net_idx != ESP_BLE_MESH_KEY_UNUSED &&
        binding_is_locally_valid(net_idx, app_idx)) {
        set_runtime_binding(net_idx, app_idx, true);
        esp_err_t err = binding_storage_save(net_idx, app_idx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to persist inferred binding: %s",
                     esp_err_to_name(err));
        }
        ESP_LOGW(TAG, "inferred binding from the only local subnet: "
                 "net=0x%03x app=0x%03x", net_idx, app_idx);
        return;
    }

    set_runtime_binding(ESP_BLE_MESH_KEY_UNUSED,
                        ESP_BLE_MESH_KEY_UNUSED, false);
    ESP_LOGW(TAG, "provisioned node has no unambiguous persisted Vendor "
             "Model binding; rebind its AppKey if multiple subnets exist");
}

static bool is_our_vendor_binding(uint16_t element_addr,
                                  uint16_t company_id,
                                  uint16_t model_id)
{
    return element_addr == s_elements[0].element_addr &&
           company_id == BLE_MESH_IMAGE_COMPANY_ID &&
           model_id == BLE_MESH_IMAGE_SOURCE_MODEL_ID;
}

static void provisioning_callback(esp_ble_mesh_prov_cb_event_t event,
                                  esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "provisioning callback registered, err=%d",
                 param->prov_register_comp.err_code);
        break;
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "provisioned net=0x%03x addr=0x%04x",
                 param->node_prov_complete.net_idx,
                 param->node_prov_complete.addr);
        refresh_ready_state();
        break;
    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        binding_storage_clear();
        set_runtime_binding(ESP_BLE_MESH_KEY_UNUSED,
                            ESP_BLE_MESH_KEY_UNUSED, false);
        ESP_LOGW(TAG, "node provisioning reset");
        {
            esp_err_t err = esp_ble_mesh_node_prov_enable(
                (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV |
                                             ESP_BLE_MESH_PROV_GATT));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "failed to reopen provisioning bearers: %s",
                         esp_err_to_name(err));
            }
        }
        break;
    default:
        break;
    }
}

static void config_server_callback(esp_ble_mesh_cfg_server_cb_event_t event,
                                   esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT || param == NULL) {
        return;
    }

    switch (param->ctx.recv_op) {
    case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
    case ESP_BLE_MESH_MODEL_OP_APP_KEY_UPDATE: {
        const esp_ble_mesh_state_change_cfg_appkey_add_t *value =
            &param->value.state_change.appkey_add;
        app_net_cache_put(value->app_idx, value->net_idx);
        ESP_LOGI(TAG, "AppKey mapped net=0x%03x app=0x%03x",
                 value->net_idx, value->app_idx);
        break;
    }
    case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND: {
        const esp_ble_mesh_state_change_cfg_model_app_bind_t *value =
            &param->value.state_change.mod_app_bind;
        if (!is_our_vendor_binding(value->element_addr,
                                   value->company_id,
                                   value->model_id)) {
            break;
        }

        uint16_t net_idx = app_net_cache_get(value->app_idx);
        if (net_idx == ESP_BLE_MESH_KEY_UNUSED) {
            net_idx = unique_local_net_idx();
        }
        if (net_idx == ESP_BLE_MESH_KEY_UNUSED ||
            esp_ble_mesh_node_get_local_net_key(net_idx) == NULL ||
            esp_ble_mesh_node_get_local_app_key(value->app_idx) == NULL) {
            ESP_LOGE(TAG, "cannot resolve NetKey for bound AppKey 0x%03x",
                     value->app_idx);
            set_runtime_binding(ESP_BLE_MESH_KEY_UNUSED,
                                ESP_BLE_MESH_KEY_UNUSED, false);
            break;
        }

        set_runtime_binding(net_idx, value->app_idx, true);
        esp_err_t err = binding_storage_save(net_idx, value->app_idx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to persist binding: %s",
                     esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "Vendor Model bound net=0x%03x app=0x%03x",
                 net_idx, value->app_idx);
        break;
    }
    case ESP_BLE_MESH_MODEL_OP_MODEL_APP_UNBIND: {
        const esp_ble_mesh_state_change_cfg_model_app_unbind_t *value =
            &param->value.state_change.mod_app_unbind;
        if (!is_our_vendor_binding(value->element_addr,
                                   value->company_id,
                                   value->model_id)) {
            break;
        }
        binding_storage_clear();
        set_runtime_binding(ESP_BLE_MESH_KEY_UNUSED,
                            ESP_BLE_MESH_KEY_UNUSED, false);
        restore_binding_from_mesh();
        break;
    }
    case ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET: {
        const esp_ble_mesh_state_change_cfg_mod_pub_set_t *value =
            &param->value.state_change.mod_pub_set;
        if (!is_our_vendor_binding(value->element_addr,
                                   value->company_id,
                                   value->model_id)) {
            break;
        }
        portENTER_CRITICAL(&s_state_lock);
        ++s_binding_generation;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(TAG,
                 "publication destination=0x%04x app=0x%03x ttl=0x%02x",
                 value->pub_addr, value->app_idx, value->pub_ttl);
        refresh_ready_state();
        break;
    }
    case ESP_BLE_MESH_MODEL_OP_MODEL_PUB_VIRTUAL_ADDR_SET: {
        const esp_ble_mesh_state_change_cfg_mod_pub_va_set_t *value =
            &param->value.state_change.mod_pub_va_set;
        if (!is_our_vendor_binding(value->element_addr,
                                   value->company_id,
                                   value->model_id)) {
            break;
        }
        portENTER_CRITICAL(&s_state_lock);
        ++s_binding_generation;
        portEXIT_CRITICAL(&s_state_lock);
        /* Image recovery requires a single unicast Gateway. */
        refresh_ready_state();
        break;
    }
    case ESP_BLE_MESH_MODEL_OP_APP_KEY_DELETE: {
        const esp_ble_mesh_state_change_cfg_appkey_delete_t *value =
            &param->value.state_change.appkey_delete;
        uint16_t active_app;
        app_net_cache_remove(value->app_idx);
        portENTER_CRITICAL(&s_state_lock);
        active_app = s_app_idx;
        portEXIT_CRITICAL(&s_state_lock);
        if (active_app == value->app_idx) {
            binding_storage_clear();
            set_runtime_binding(ESP_BLE_MESH_KEY_UNUSED,
                                ESP_BLE_MESH_KEY_UNUSED, false);
            restore_binding_from_mesh();
        }
        break;
    }
    case ESP_BLE_MESH_MODEL_OP_NET_KEY_DELETE: {
        const uint16_t deleted_net =
            param->value.state_change.netkey_delete.net_idx;
        uint16_t active_net;
        portENTER_CRITICAL(&s_state_lock);
        active_net = s_net_idx;
        portEXIT_CRITICAL(&s_state_lock);
        if (active_net == deleted_net) {
            binding_storage_clear();
            set_runtime_binding(ESP_BLE_MESH_KEY_UNUSED,
                                ESP_BLE_MESH_KEY_UNUSED, false);
            restore_binding_from_mesh();
        }
        break;
    }
    default:
        break;
    }
}

static void mark_transport_fault(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_transport_healthy = false;
    s_transport_restart_pending = true;
    portEXIT_CRITICAL(&s_state_lock);
    refresh_ready_state();
}

static void restart_after_transport_fault(void)
{
    bool restart_pending;

    portENTER_CRITICAL(&s_state_lock);
    restart_pending = s_transport_restart_pending;
    portEXIT_CRITICAL(&s_state_lock);

    if (!restart_pending) {
        return;
    }

    /*
     * A Mesh model-send completion has no application correlation ID.  Keep
     * the source NOT_READY and do not start another send in this boot, so a
     * late completion can only be ignored by custom_model_callback().  The
     * frame_done callback has already returned when this function is called;
     * give the SDIO status worker time to send FAILED before rebooting.  A
     * full software restart is the recovery boundary that guarantees the late
     * completion cannot be attributed to a future frame.  esp_restart() does
     * not erase NVS, so Mesh provisioning and the persisted binding survive.
     */
    ESP_LOGW(TAG,
             "BLE transport recovery restart in %u ms (NVS preserved)",
             TRANSPORT_RESTART_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(TRANSPORT_RESTART_DELAY_MS));
    esp_restart();
}

static bool snapshot_route(mesh_route_t *route)
{
    bool ready;

    if (route == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_state_lock);
    ready = s_ready;
    if (ready) {
        route->net_idx = s_net_idx;
        route->app_idx = s_app_idx;
        route->destination = s_image_publication.publish_addr;
        route->ttl = s_image_publication.ttl;
        route->binding_generation = s_binding_generation;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return ready;
}

static bool route_still_matches(const mesh_route_t *route)
{
    bool matches;

    if (route == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_state_lock);
    matches = s_ready && s_net_idx == route->net_idx &&
              s_app_idx == route->app_idx &&
              s_binding_generation == route->binding_generation &&
              s_image_publication.publish_addr == route->destination &&
              s_image_publication.app_idx == route->app_idx;
    portEXIT_CRITICAL(&s_state_lock);
    return matches;
}

static bool deadline_expired(int64_t deadline_us)
{
    return esp_timer_get_time() >= deadline_us;
}

static TickType_t response_wait_ticks(int64_t deadline_us)
{
    int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    uint32_t wait_ms = (uint32_t)((remaining_us + 999) / 1000);
    if (wait_ms > RESPONSE_TIMEOUT_MS) {
        wait_ms = RESPONSE_TIMEOUT_MS;
    }
    return pdMS_TO_TICKS(wait_ms);
}

static void clear_gateway_response(void)
{
    while (xSemaphoreTake(s_response_sem, 0) == pdTRUE) {
    }

    portENTER_CRITICAL(&s_state_lock);
    s_gateway_response = RESPONSE_NONE;
    s_reject_reason = 0;
    portEXIT_CRITICAL(&s_state_lock);
}

static gateway_response_t wait_gateway_response(int64_t deadline_us,
                                                uint8_t *reject_reason)
{
    gateway_response_t response;

    (void)xSemaphoreTake(s_response_sem, response_wait_ticks(deadline_us));
    portENTER_CRITICAL(&s_state_lock);
    response = s_gateway_response;
    s_gateway_response = RESPONSE_NONE;
    if (reject_reason != NULL) {
        *reject_reason = s_reject_reason;
    }
    s_reject_reason = 0;
    portEXIT_CRITICAL(&s_state_lock);
    return response;
}

static gateway_response_t take_data_phase_interrupt(uint8_t *reject_reason)
{
    gateway_response_t response = RESPONSE_NONE;

    portENTER_CRITICAL(&s_state_lock);
    if (s_gateway_response == RESPONSE_COMPLETE ||
        s_gateway_response == RESPONSE_RESTART ||
        s_gateway_response == RESPONSE_REJECT) {
        response = s_gateway_response;
        s_gateway_response = RESPONSE_NONE;
        if (reject_reason != NULL) {
            *reject_reason = s_reject_reason;
        }
        s_reject_reason = 0;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return response;
}

static esp_err_t send_mesh_message(uint32_t opcode,
                                   const uint8_t *payload,
                                   uint16_t payload_len,
                                   const mesh_route_t *route,
                                   int64_t deadline_us)
{
    if (payload == NULL || payload_len == 0U || route == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx = route->net_idx,
        .app_idx = route->app_idx,
        .addr = route->destination,
        .send_ttl = route->ttl,
    };

    esp_err_t last_error = ESP_FAIL;
    for (unsigned int attempt = 1; attempt <= LOCAL_SEND_RETRIES; ++attempt) {
        if (deadline_expired(deadline_us)) {
            return ESP_ERR_TIMEOUT;
        }
        if (!route_still_matches(route)) {
            return ESP_ERR_INVALID_STATE;
        }

        while (xSemaphoreTake(s_send_done_sem, 0) == pdTRUE) {
        }

        portENTER_CRITICAL(&s_state_lock);
        s_send_result = ESP_FAIL;
        s_send_completed = false;
        s_send_waiting = true;
        s_send_opcode = opcode;
        portEXIT_CRITICAL(&s_state_lock);

        esp_err_t err = esp_ble_mesh_server_model_send_msg(
            &s_vendor_models[0], &ctx, opcode, payload_len,
            (uint8_t *)payload);
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_state_lock);
            s_send_waiting = false;
            portEXIT_CRITICAL(&s_state_lock);
            last_error = err;
            ESP_LOGW(TAG, "model-send enqueue failed %u/%u: %s",
                     attempt, LOCAL_SEND_RETRIES, esp_err_to_name(err));
            if (attempt < LOCAL_SEND_RETRIES) {
                vTaskDelay(pdMS_TO_TICKS(ENQUEUE_RETRY_DELAY_MS));
            }
            continue;
        }

        TickType_t wait_ticks = pdMS_TO_TICKS(LOCAL_SEND_TIMEOUT_MS);
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            wait_ticks = 0;
        } else {
            TickType_t remaining_ticks =
                pdMS_TO_TICKS((uint32_t)((remaining_us + 999) / 1000));
            if (remaining_ticks < wait_ticks) {
                wait_ticks = remaining_ticks;
            }
        }

        BaseType_t notified = xSemaphoreTake(s_send_done_sem, wait_ticks);
        bool completed;
        esp_err_t completion_result;

        portENTER_CRITICAL(&s_state_lock);
        completed = s_send_completed;
        completion_result = s_send_result;
        s_send_waiting = false;
        portEXIT_CRITICAL(&s_state_lock);

        if ((notified == pdTRUE || completed) && completed) {
            if (completion_result == ESP_OK) {
                return ESP_OK;
            }
            if (completion_result != -EBUSY) {
                return completion_result;
            }
            last_error = completion_result;
            if (attempt < LOCAL_SEND_RETRIES) {
                vTaskDelay(pdMS_TO_TICKS(BUSY_RETRY_DELAY_MS));
            }
            continue;
        }

        ESP_LOGE(TAG,
                 "model-send completion timed out; transport recovery queued");
        mark_transport_fault();
        return ESP_ERR_TIMEOUT;
    }

    return last_error;
}

static uint16_t image_chunk_count(size_t jpeg_len)
{
    return (uint16_t)((jpeg_len + BLE_MESH_IMAGE_DATA_BYTES - 1U) /
                      BLE_MESH_IMAGE_DATA_BYTES);
}

static esp_err_t send_open(const image_job_t *job,
                           const mesh_route_t *route,
                           int64_t deadline_us)
{
    uint8_t packet[sizeof(ble_mesh_image_open_t)];
    ble_mesh_image_put_le16(packet, job->ble_frame_id);
    ble_mesh_image_put_le64(packet + 2, job->detected_at_ms);
    ble_mesh_image_put_le16(packet + 10, (uint16_t)job->len);
    ble_mesh_image_put_le32(packet + 12, job->jpeg_crc32);
    return send_mesh_message(MESH_OPCODE_OPEN, packet, sizeof(packet), route,
                             deadline_us);
}

static esp_err_t send_chunk(const image_job_t *job,
                            uint8_t chunk_index,
                            uint16_t total_chunks,
                            const mesh_route_t *route,
                            int64_t deadline_us,
                            bool retransmission)
{
    const size_t offset = (size_t)chunk_index * BLE_MESH_IMAGE_DATA_BYTES;
    if (job == NULL || chunk_index >= total_chunks || offset >= job->len) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t copy_len = job->len - offset;
    if (copy_len > BLE_MESH_IMAGE_DATA_BYTES) {
        copy_len = BLE_MESH_IMAGE_DATA_BYTES;
    }

    uint8_t packet[sizeof(ble_mesh_image_data_header_t) +
                   BLE_MESH_IMAGE_DATA_BYTES];
    ble_mesh_image_put_le16(packet, job->ble_frame_id);
    packet[2] = chunk_index;
    memcpy(packet + sizeof(ble_mesh_image_data_header_t),
           job->jpeg + offset, copy_len);

    if (retransmission) {
        ESP_LOGW(TAG, "retransmit frame=%u chunk=%u/%u",
                 job->ble_frame_id, (unsigned int)chunk_index + 1U,
                 total_chunks);
    }

    return send_mesh_message(
        MESH_OPCODE_DATA, packet,
        (uint16_t)(sizeof(ble_mesh_image_data_header_t) + copy_len), route,
        deadline_us);
}

static esp_err_t send_end(const image_job_t *job,
                          const mesh_route_t *route,
                          int64_t deadline_us)
{
    uint8_t packet[sizeof(ble_mesh_image_frame_t)];
    ble_mesh_image_put_le16(packet, job->ble_frame_id);
    return send_mesh_message(MESH_OPCODE_END, packet, sizeof(packet), route,
                             deadline_us);
}

static void begin_active_frame(const image_job_t *job,
                               uint16_t total_chunks,
                               const mesh_route_t *route)
{
    while (xSemaphoreTake(s_response_sem, 0) == pdTRUE) {
    }

    portENTER_CRITICAL(&s_state_lock);
    memset(s_nack_bitmap, 0, sizeof(s_nack_bitmap));
    s_gateway_response = RESPONSE_NONE;
    s_reject_reason = 0;
    s_active_frame_id = job->ble_frame_id;
    s_active_total_chunks = total_chunks;
    s_active_net_idx = route->net_idx;
    s_active_app_idx = route->app_idx;
    s_active_destination = route->destination;
    s_active_binding_generation = route->binding_generation;
    s_active = true;
    portEXIT_CRITICAL(&s_state_lock);
}

static void end_active_frame(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_active = false;
    s_active_total_chunks = 0;
    s_active_net_idx = ESP_BLE_MESH_KEY_UNUSED;
    s_active_app_idx = ESP_BLE_MESH_KEY_UNUSED;
    s_active_destination = 0;
    s_active_binding_generation = 0;
    s_gateway_response = RESPONSE_NONE;
    s_reject_reason = 0;
    memset(s_nack_bitmap, 0, sizeof(s_nack_bitmap));
    portEXIT_CRITICAL(&s_state_lock);

    while (xSemaphoreTake(s_response_sem, 0) == pdTRUE) {
    }
}

static bool pop_pending_nack(uint8_t *chunk_index)
{
    bool found = false;

    portENTER_CRITICAL(&s_state_lock);
    for (uint16_t i = 0; i < s_active_total_chunks; ++i) {
        const uint8_t mask = (uint8_t)(1U << (i & 7U));
        if ((s_nack_bitmap[i >> 3] & mask) != 0U) {
            s_nack_bitmap[i >> 3] &= (uint8_t)~mask;
            *chunk_index = (uint8_t)i;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    return found;
}

static esp_err_t send_pending_nacks(const image_job_t *job,
                                    uint16_t total_chunks,
                                    const mesh_route_t *route,
                                    int64_t deadline_us)
{
    uint8_t chunk_index;
    while (pop_pending_nack(&chunk_index)) {
        esp_err_t err = send_chunk(job, chunk_index, total_chunks, route,
                                   deadline_us, true);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static void busy_full_jitter(unsigned int attempt, int64_t deadline_us)
{
    static const uint32_t caps_ms[OPEN_ATTEMPTS] = {
        1000U, 2000U, 4000U, 8000U, 10000U,
    };
    const size_t index = attempt < ARRAY_SIZE(caps_ms)
                             ? attempt
                             : ARRAY_SIZE(caps_ms) - 1U;
    uint32_t delay_ms = esp_random() % (caps_ms[index] + 1U);
    const int64_t remaining_ms =
        (deadline_us - esp_timer_get_time()) / 1000;
    if (remaining_ms <= 0) {
        return;
    }
    if ((int64_t)delay_ms > remaining_ms) {
        delay_ms = (uint32_t)remaining_ms;
    }
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static session_result_t open_session(const image_job_t *job,
                                     const mesh_route_t *route,
                                     int64_t deadline_us,
                                     esp_err_t *error)
{
    for (unsigned int attempt = 0; attempt < OPEN_ATTEMPTS; ++attempt) {
        clear_gateway_response();
        esp_err_t err = send_open(job, route, deadline_us);
        if (err != ESP_OK) {
            *error = err;
            return SESSION_RESULT_FAILED;
        }

        uint8_t reject_reason = 0;
        gateway_response_t response =
            wait_gateway_response(deadline_us, &reject_reason);
        switch (response) {
        case RESPONSE_ACCEPT:
            return SESSION_RESULT_ACCEPTED;
        case RESPONSE_COMPLETE:
            return SESSION_RESULT_COMPLETE;
        case RESPONSE_RESTART:
            return SESSION_RESULT_RESTART;
        case RESPONSE_REJECT:
            ESP_LOGE(TAG, "Gateway rejected OPEN frame=%u reason=%u",
                     job->ble_frame_id, reject_reason);
            *error = ESP_FAIL;
            return SESSION_RESULT_FAILED;
        case RESPONSE_BUSY:
            busy_full_jitter(attempt, deadline_us);
            break;
        case RESPONSE_NONE:
        case RESPONSE_NACK:
        default:
            break;
        }

        if (deadline_expired(deadline_us)) {
            break;
        }
    }

    *error = ESP_ERR_TIMEOUT;
    return SESSION_RESULT_FAILED;
}

static session_result_t finish_session(const image_job_t *job,
                                       uint16_t total_chunks,
                                       const mesh_route_t *route,
                                       int64_t deadline_us,
                                       esp_err_t *error)
{
    for (unsigned int round = 0; round < REPAIR_ROUNDS; ++round) {
        esp_err_t err = send_pending_nacks(job, total_chunks, route,
                                           deadline_us);
        if (err != ESP_OK) {
            *error = err;
            return SESSION_RESULT_FAILED;
        }

        clear_gateway_response();
        err = send_end(job, route, deadline_us);
        if (err != ESP_OK) {
            *error = err;
            return SESSION_RESULT_FAILED;
        }

        uint8_t reject_reason = 0;
        gateway_response_t response =
            wait_gateway_response(deadline_us, &reject_reason);
        switch (response) {
        case RESPONSE_COMPLETE:
            return SESSION_RESULT_COMPLETE;
        case RESPONSE_RESTART:
            return SESSION_RESULT_RESTART;
        case RESPONSE_REJECT:
            ESP_LOGE(TAG, "Gateway rejected frame=%u reason=%u",
                     job->ble_frame_id, reject_reason);
            *error = ESP_FAIL;
            return SESSION_RESULT_FAILED;
        case RESPONSE_BUSY:
            busy_full_jitter(round, deadline_us);
            break;
        case RESPONSE_NACK:
        case RESPONSE_NONE:
        case RESPONSE_ACCEPT:
        default:
            break;
        }

        if (deadline_expired(deadline_us)) {
            break;
        }
    }

    *error = ESP_ERR_TIMEOUT;
    return SESSION_RESULT_FAILED;
}

static esp_err_t send_image(const image_job_t *job)
{
    mesh_route_t route;
    if (job == NULL || !snapshot_route(&route)) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t total_chunks = image_chunk_count(job->len);
    if (total_chunks == 0U || total_chunks > BLE_MESH_IMAGE_MAX_CHUNKS) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int64_t deadline_us = esp_timer_get_time() + FRAME_DEADLINE_US;
    begin_active_frame(job, total_chunks, &route);
    ESP_LOGI(TAG,
             "start P4 frame=%" PRIu32 " BLE frame=%u bytes=%u chunks=%u "
             "Gateway=0x%04x",
             job->p4_frame_id, job->ble_frame_id, (unsigned int)job->len,
             total_chunks, route.destination);

    esp_err_t err = ESP_FAIL;
    bool server_completed = false;
    for (unsigned int session = 0; session < SESSION_RESTARTS; ++session) {
        session_result_t opened =
            open_session(job, &route, deadline_us, &err);
        if (opened == SESSION_RESULT_COMPLETE) {
            err = ESP_OK;
            server_completed = true;
            break;
        }
        if (opened == SESSION_RESULT_RESTART) {
            continue;
        }
        if (opened != SESSION_RESULT_ACCEPTED) {
            break;
        }

        portENTER_CRITICAL(&s_state_lock);
        memset(s_nack_bitmap, 0, sizeof(s_nack_bitmap));
        portEXIT_CRITICAL(&s_state_lock);
        err = ESP_OK;
        bool restart_requested = false;
        for (uint16_t i = 0; i < total_chunks; ++i) {
            err = send_chunk(job, (uint8_t)i, total_chunks, &route,
                             deadline_us, false);
            if (err != ESP_OK) {
                break;
            }

            uint8_t reject_reason = 0;
            gateway_response_t interrupt =
                take_data_phase_interrupt(&reject_reason);
            if (interrupt == RESPONSE_COMPLETE) {
                server_completed = true;
                break;
            }
            if (interrupt == RESPONSE_RESTART) {
                restart_requested = true;
                break;
            }
            if (interrupt == RESPONSE_REJECT) {
                ESP_LOGE(TAG,
                         "Gateway rejected DATA frame=%u reason=%u",
                         job->ble_frame_id, reject_reason);
                err = ESP_FAIL;
                break;
            }
        }
        if (server_completed) {
            err = ESP_OK;
            break;
        }
        if (restart_requested) {
            ESP_LOGW(TAG,
                     "Gateway requested early full restart for frame=%u",
                     job->ble_frame_id);
            continue;
        }
        if (err != ESP_OK) {
            break;
        }

        session_result_t finished =
            finish_session(job, total_chunks, &route, deadline_us, &err);
        if (finished == SESSION_RESULT_COMPLETE) {
            err = ESP_OK;
            server_completed = true;
            break;
        }
        if (finished != SESSION_RESULT_RESTART) {
            break;
        }
        ESP_LOGW(TAG, "Gateway requested full restart for frame=%u",
                 job->ble_frame_id);
    }

    if (!server_completed && err == ESP_OK) {
        err = ESP_ERR_TIMEOUT;
    }
    if (deadline_expired(deadline_us) && err == ESP_OK) {
        err = ESP_ERR_TIMEOUT;
    }
    end_active_frame();
    ESP_LOGI(TAG, "finish P4 frame=%" PRIu32 " BLE frame=%u status=%s",
             job->p4_frame_id, job->ble_frame_id, esp_err_to_name(err));
    return err;
}

static void image_worker(void *arg)
{
    (void)arg;
    image_job_t job;

    for (;;) {
        if (xQueueReceive(s_job_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_err_t err = send_image(&job);
        publish_frame_done(&job, err);
        restart_after_transport_fault();
    }
}

static void custom_model_callback(esp_ble_mesh_model_cb_event_t event,
                                  esp_ble_mesh_model_cb_param_t *param)
{
    if (param == NULL) {
        return;
    }

    if (event == ESP_BLE_MESH_MODEL_SEND_COMP_EVT) {
        bool notify = false;
        portENTER_CRITICAL(&s_state_lock);
        if (param->model_send_comp.model == &s_vendor_models[0] &&
            s_send_waiting && !s_send_completed &&
            param->model_send_comp.opcode == s_send_opcode) {
            s_send_result = param->model_send_comp.err_code;
            s_send_completed = true;
            notify = true;
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (notify && s_send_done_sem != NULL) {
            xSemaphoreGive(s_send_done_sem);
        }
        return;
    }

    if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT ||
        param->model_operation.model != &s_vendor_models[0] ||
        param->model_operation.msg == NULL ||
        param->model_operation.ctx == NULL) {
        return;
    }

    const uint32_t opcode = param->model_operation.opcode;
    const uint8_t *message = param->model_operation.msg;
    const size_t length = param->model_operation.length;
    gateway_response_t response = RESPONSE_NONE;
    uint8_t reject_reason = 0;

    if (opcode == MESH_OPCODE_ACCEPT &&
        length == sizeof(ble_mesh_image_frame_t)) {
        response = RESPONSE_ACCEPT;
    } else if (opcode == MESH_OPCODE_BUSY &&
               length == sizeof(ble_mesh_image_frame_t)) {
        response = RESPONSE_BUSY;
    } else if (opcode == MESH_OPCODE_COMPLETE &&
               length == sizeof(ble_mesh_image_frame_t)) {
        response = RESPONSE_COMPLETE;
    } else if (opcode == MESH_OPCODE_RESTART &&
               length == sizeof(ble_mesh_image_frame_t)) {
        response = RESPONSE_RESTART;
    } else if (opcode == MESH_OPCODE_REJECT &&
               length == sizeof(ble_mesh_image_reject_t)) {
        response = RESPONSE_REJECT;
        reject_reason = message[2];
    } else if (opcode == MESH_OPCODE_NACK &&
               length >= sizeof(ble_mesh_image_nack_header_t) + 1U &&
               length <= sizeof(ble_mesh_image_nack_header_t) +
                             BLE_MESH_IMAGE_NACK_BITMAP_MAX) {
        response = RESPONSE_NACK;
    } else {
        ESP_LOGW(TAG, "discard malformed/unknown response opcode=0x%08" PRIx32
                      " len=%u",
                 opcode, (unsigned int)length);
        return;
    }

    const uint16_t frame_id = ble_mesh_image_get_le16(message);
    esp_ble_mesh_msg_ctx_t *ctx = param->model_operation.ctx;
    bool accepted = false;

    portENTER_CRITICAL(&s_state_lock);
    if (s_active && frame_id == s_active_frame_id && s_ready &&
        s_active_binding_generation == s_binding_generation &&
        ctx->addr == s_active_destination &&
        ctx->net_idx == s_active_net_idx &&
        ctx->app_idx == s_active_app_idx) {
        if (response == RESPONSE_NACK) {
            const uint8_t base = message[2];
            bool requested = false;
            for (size_t byte_index = 0;
                 byte_index < length - sizeof(ble_mesh_image_nack_header_t);
                 ++byte_index) {
                const uint8_t bits =
                    message[sizeof(ble_mesh_image_nack_header_t) + byte_index];
                for (unsigned int bit = 0; bit < 8U; ++bit) {
                    if ((bits & (uint8_t)(1U << bit)) == 0U) {
                        continue;
                    }
                    const uint16_t chunk_index =
                        (uint16_t)base + (uint16_t)(byte_index * 8U) + bit;
                    if (chunk_index >= s_active_total_chunks) {
                        continue;
                    }
                    s_nack_bitmap[chunk_index >> 3] |=
                        (uint8_t)(1U << (chunk_index & 7U));
                    requested = true;
                }
            }
            accepted = requested;
        } else {
            accepted = true;
        }

        if (accepted && s_gateway_response != RESPONSE_COMPLETE &&
            s_gateway_response != RESPONSE_REJECT) {
            const bool incoming_terminal = response == RESPONSE_COMPLETE ||
                                           response == RESPONSE_REJECT;
            const bool existing_restart =
                s_gateway_response == RESPONSE_RESTART;
            if (incoming_terminal || response == RESPONSE_RESTART ||
                !existing_restart) {
                s_gateway_response = response;
                s_reject_reason = reject_reason;
            }
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!accepted) {
        ESP_LOGW(TAG,
                 "discard stale/invalid Gateway response frame=%u source=0x%04x",
                 frame_id, ctx->addr);
        return;
    }

    if (s_response_sem != NULL) {
        xSemaphoreGive(s_response_sem);
    }
}

#if defined(CONFIG_BT_NIMBLE_ENABLED)
static void nimble_mesh_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset, reason=%d", reason);
}

static void nimble_mesh_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_bt_addr_type);
    }
    if (rc == 0) {
        rc = ble_hs_id_copy_addr(s_bt_addr_type, s_bt_addr, NULL);
    }
    s_bt_sync_result = (rc == 0) ? ESP_OK : ESP_FAIL;
    if (s_bt_sync_sem != NULL) {
        xSemaphoreGive(s_bt_sync_sem);
    }
}

static void nimble_mesh_host_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t local_bluetooth_init(void)
{
    s_bt_sync_sem = xSemaphoreCreateBinary();
    if (s_bt_sync_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = nimble_mesh_reset;
    ble_hs_cfg.sync_cb = nimble_mesh_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

#if CONFIG_BLE_MESH_USE_BLE_50
    extern void bt_mesh_gatts_svcs_add(void);
    bt_mesh_gatts_svcs_add();
#endif

    nimble_port_freertos_init(nimble_mesh_host_task);
    if (xSemaphoreTake(s_bt_sync_sem,
                       pdMS_TO_TICKS(BT_SYNC_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_bt_sync_result != ESP_OK) {
        return s_bt_sync_result;
    }
    memcpy(s_dev_uuid + 2, s_bt_addr, sizeof(s_bt_addr));
    return ESP_OK;
}
#elif defined(CONFIG_BT_BLUEDROID_ENABLED)
static esp_err_t local_bluetooth_init(void)
{
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        return err;
    }

    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&config);
    if (err == ESP_OK) {
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    }
    if (err == ESP_OK) {
        err = esp_bluedroid_init();
    }
    if (err == ESP_OK) {
        err = esp_bluedroid_enable();
    }
    if (err == ESP_OK) {
        memcpy(s_dev_uuid + 2, esp_bt_dev_get_address(), 6U);
    }
    return err;
}
#else
static esp_err_t local_bluetooth_init(void)
{
    ESP_LOGE(TAG, "enable CONFIG_BT_NIMBLE_ENABLED or Bluedroid");
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

static esp_err_t create_runtime_objects(void)
{
    s_job_queue = xQueueCreate(1, sizeof(image_job_t));
    s_send_done_sem = xSemaphoreCreateBinary();
    s_response_sem = xSemaphoreCreateBinary();
    if (s_job_queue == NULL || s_send_done_sem == NULL ||
        s_response_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t fail_init(esp_err_t err)
{
    portENTER_CRITICAL(&s_state_lock);
    s_initializing = false;
    s_init_failed = true;
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGE(TAG, "BLE image source init failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t ble_mesh_image_source_init(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_initialized) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    if (s_initializing || s_init_failed) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_initializing = true;
    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        /* Never erase shared NVS implicitly; let the application decide. */
        return fail_init(err);
    }

    err = create_runtime_objects();
    if (err != ESP_OK) {
        return fail_init(err);
    }

    err = local_bluetooth_init();
    if (err != ESP_OK) {
        return fail_init(err);
    }

    /* A fresh boot must not deterministically reuse BLE frame 1 while the
     * Gateway may still hold a short completed-frame cache. Per-frame NVS
     * writes would add wear, so seed the in-RAM counter once per boot. */
    uint16_t frame_seed = (uint16_t)esp_random();
    if (frame_seed == 0U) {
        frame_seed = 1U;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_next_frame_id = frame_seed;
    portEXIT_CRITICAL(&s_state_lock);

    err = esp_ble_mesh_register_prov_callback(provisioning_callback);
    if (err == ESP_OK) {
        err = esp_ble_mesh_register_config_server_callback(
            config_server_callback);
    }
    if (err == ESP_OK) {
        err = esp_ble_mesh_register_custom_model_callback(
            custom_model_callback);
    }
    if (err == ESP_OK) {
        err = esp_ble_mesh_init(&s_provision, &s_composition);
    }
    if (err != ESP_OK) {
        return fail_init(err);
    }

    if (xTaskCreate(image_worker, "ble_img_worker",
                    IMAGE_WORKER_STACK_BYTES, NULL,
                    IMAGE_WORKER_PRIORITY, &s_worker_task) != pdPASS) {
        return fail_init(ESP_ERR_NO_MEM);
    }

    err = esp_ble_mesh_node_prov_enable(
        (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV |
                                     ESP_BLE_MESH_PROV_GATT));
    if (err != ESP_OK) {
        return fail_init(err);
    }

    portENTER_CRITICAL(&s_state_lock);
    s_initialized = true;
    s_initializing = false;
    portEXIT_CRITICAL(&s_state_lock);

    restore_binding_from_mesh();
    refresh_ready_state();
    ESP_LOGI(TAG, "BLE Mesh image source initialized");
    return ESP_OK;
}

esp_err_t ble_mesh_image_source_submit(const uint8_t *jpeg,
                                       size_t len,
                                       uint32_t p4_frame_id,
                                       uint64_t detected_at_ms,
                                       uint32_t jpeg_crc32,
                                       uint16_t *ble_frame_id)
{
    if (jpeg == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > BLE_MESH_IMAGE_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    image_job_t job = {
        .jpeg = jpeg,
        .len = len,
        .p4_frame_id = p4_frame_id,
        .detected_at_ms = detected_at_ms,
        .jpeg_crc32 = jpeg_crc32,
    };

    portENTER_CRITICAL(&s_state_lock);
    if (!s_initialized || !s_ready || s_outstanding) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    job.ble_frame_id = s_next_frame_id++;
    if (s_next_frame_id == 0U) {
        s_next_frame_id = 1U;
    }
    s_outstanding = true;
    portEXIT_CRITICAL(&s_state_lock);

    if (xQueueSend(s_job_queue, &job, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_state_lock);
        s_outstanding = false;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_TIMEOUT;
    }

    if (ble_frame_id != NULL) {
        *ble_frame_id = job.ble_frame_id;
    }
    return ESP_OK;
}
