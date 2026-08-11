#include "mesh_image_gateway.h"

#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"

#include "ble_mesh_image_protocol.h"
#include "image_reassembly.h"

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if defined(CONFIG_BT_NIMBLE_ENABLED)
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "store/config/ble_store_config.h"
extern void ble_store_config_init(void);
#elif defined(CONFIG_BT_BLUEDROID_ENABLED)
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#endif

#define TAG "mesh_gateway"

#define SERVER_UNICAST_ADDR       UINT16_C(0x0001)
#define FIRST_NODE_ADDR           UINT16_C(0x0002)
#define PRIMARY_NET_IDX           ESP_BLE_MESH_KEY_PRIMARY
#define IMAGE_APP_IDX             UINT16_C(0x0000)
#define CONFIG_TIMEOUT_MS         4000
#define CONFIG_RETRIES            3U
#define CONTROL_TTL               3U
#define TIMEOUT_POLL_MS           1000U
#define BT_SYNC_TIMEOUT_MS        10000U
#define KEY_NVS_NAMESPACE         "mesh_server"
#define KEY_NVS_APP_KEY           "app_key"

#define IMAGE_OPCODE(octet) \
    ESP_BLE_MESH_MODEL_OP_3((octet), BLE_MESH_IMAGE_COMPANY_ID)

typedef struct {
    bool used;
    uint8_t uuid[16];
    uint16_t unicast;
    uint8_t element_count;
    uint32_t pending_opcode;
    uint8_t retries;
} configured_node_t;

static uint8_t s_prov_uuid[16] = {0x47U, 0x57U};
static uint8_t s_app_key[16];
static bool s_provisioning_enabled;
static bool s_initialized;
#if CONFIG_SERVER_TEST_DROP_CHUNK_INDEX >= 0
static bool s_test_drop_done;
#endif

static esp_ble_mesh_client_t s_config_client;
static esp_ble_mesh_client_t s_vendor_client;
static esp_ble_mesh_cfg_srv_t s_config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
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
    .default_ttl = CONTROL_TTL,
};

static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&s_config_client),
};

static esp_ble_mesh_model_op_t s_vendor_ops[] = {
    ESP_BLE_MESH_MODEL_OP(IMAGE_OPCODE(BLE_MESH_IMAGE_OP_OPEN),
                          sizeof(ble_mesh_image_open_t)),
    ESP_BLE_MESH_MODEL_OP(IMAGE_OPCODE(BLE_MESH_IMAGE_OP_DATA),
                          sizeof(ble_mesh_image_data_header_t) + 1U),
    ESP_BLE_MESH_MODEL_OP(IMAGE_OPCODE(BLE_MESH_IMAGE_OP_END),
                          sizeof(ble_mesh_image_frame_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t s_vendor_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(BLE_MESH_IMAGE_COMPANY_ID,
                              BLE_MESH_IMAGE_GATEWAY_MODEL_ID,
                              s_vendor_ops, NULL, &s_vendor_client),
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
    .prov_uuid = s_prov_uuid,
    .prov_unicast_addr = SERVER_UNICAST_ADDR,
    .prov_start_address = FIRST_NODE_ADDR,
    .prov_attention = 0,
    .prov_algorithm = 0,
    .prov_pub_key_oob = 0,
    .prov_static_oob_val = NULL,
    .prov_static_oob_len = 0,
    .flags = 0,
    .iv_index = 0,
};

static configured_node_t s_nodes[CONFIG_SERVER_MAX_NODES];
static image_reassembly_t s_reassembly;
static SemaphoreHandle_t s_reassembly_mutex;
static bool s_idle_work_reserved;

static portMUX_TYPE s_callback_lock = portMUX_INITIALIZER_UNLOCKED;
static mesh_image_gateway_image_cb_t s_image_callback;
static void *s_image_callback_ctx;
static mesh_image_gateway_time_provider_t s_time_provider;
static void *s_time_provider_ctx;

static esp_err_t send_node_config(configured_node_t *node, uint32_t opcode);

#if defined(CONFIG_BT_NIMBLE_ENABLED)
static SemaphoreHandle_t s_bt_sync_sem;
static esp_err_t s_bt_sync_result = ESP_FAIL;
static uint8_t s_bt_addr_type;
static uint8_t s_bt_addr[6];
#endif

esp_err_t mesh_image_gateway_register_image_callback(
    mesh_image_gateway_image_cb_t callback, void *user_ctx)
{
    portENTER_CRITICAL(&s_callback_lock);
    s_image_callback = callback;
    s_image_callback_ctx = user_ctx;
    portEXIT_CRITICAL(&s_callback_lock);
    return ESP_OK;
}

esp_err_t mesh_image_gateway_set_time_provider(
    mesh_image_gateway_time_provider_t provider, void *user_ctx)
{
    portENTER_CRITICAL(&s_callback_lock);
    s_time_provider = provider;
    s_time_provider_ctx = user_ctx;
    portEXIT_CRITICAL(&s_callback_lock);
    return ESP_OK;
}

bool mesh_image_gateway_is_receiving(void)
{
    if (s_reassembly_mutex == NULL) {
        return false;
    }
    bool receiving;
    xSemaphoreTake(s_reassembly_mutex, portMAX_DELAY);
    receiving = s_reassembly.active;
    xSemaphoreGive(s_reassembly_mutex);
    return receiving;
}

bool mesh_image_gateway_try_begin_idle_work(void)
{
    if (s_reassembly_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_reassembly_mutex, portMAX_DELAY);
    bool reserved = !s_reassembly.active && !s_idle_work_reserved;
    if (reserved) {
        s_idle_work_reserved = true;
    }
    xSemaphoreGive(s_reassembly_mutex);
    return reserved;
}

void mesh_image_gateway_end_idle_work(void)
{
    if (s_reassembly_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_reassembly_mutex, portMAX_DELAY);
    s_idle_work_reserved = false;
    xSemaphoreGive(s_reassembly_mutex);
}

static uint64_t monotonic_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static configured_node_t *find_node(uint16_t unicast)
{
    for (size_t i = 0; i < ARRAY_SIZE(s_nodes); ++i) {
        if (s_nodes[i].used && s_nodes[i].unicast == unicast) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

static configured_node_t *store_node(const uint8_t uuid[16],
                                     uint16_t unicast,
                                     uint8_t element_count)
{
    configured_node_t *entry = find_node(unicast);
    if (entry == NULL) {
        for (size_t i = 0; i < ARRAY_SIZE(s_nodes); ++i) {
            if (!s_nodes[i].used) {
                entry = &s_nodes[i];
                break;
            }
        }
    }
    if (entry == NULL) {
        return NULL;
    }

    *entry = (configured_node_t) {
        .used = true,
        .unicast = unicast,
        .element_count = element_count,
    };
    memcpy(entry->uuid, uuid, 16U);
    return entry;
}

static void restore_provisioned_nodes(void)
{
    const esp_ble_mesh_node_t **table =
        esp_ble_mesh_provisioner_get_node_table_entry();
    if (table == NULL) {
        return;
    }
    for (size_t i = 0; i < CONFIG_BLE_MESH_MAX_PROV_NODES; ++i) {
        const esp_ble_mesh_node_t *stored = table[i];
        if (stored == NULL || stored->dev_uuid[0] != 0x32U ||
            stored->dev_uuid[1] != 0x10U) {
            continue;
        }
        configured_node_t *node = store_node(
            stored->dev_uuid, stored->unicast_addr, stored->element_num);
        if (node == NULL) {
            ESP_LOGE(TAG, "runtime node table full while restoring 0x%04x",
                     stored->unicast_addr);
            continue;
        }
        ESP_LOGI(TAG, "restored C6 addr=0x%04x for idempotent config",
                 node->unicast);
    }
}

static void resume_node_configuration(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(s_nodes); ++i) {
        configured_node_t *node = &s_nodes[i];
        if (!node->used || node->pending_opcode != 0U) {
            continue;
        }
        node->retries = 0U;
        esp_err_t err = send_node_config(
            node, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "resume AppKey Add failed addr=0x%04x: %s",
                     node->unicast, esp_err_to_name(err));
        }
    }
}

static esp_err_t send_node_config(configured_node_t *node, uint32_t opcode)
{
    if (node == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_ble_mesh_client_common_param_t common = {
        .opcode = opcode,
        .model = s_config_client.model,
        .ctx = {
            .net_idx = PRIMARY_NET_IDX,
            .app_idx = IMAGE_APP_IDX,
            .addr = node->unicast,
            .send_ttl = CONTROL_TTL,
        },
        .msg_timeout = CONFIG_TIMEOUT_MS,
    };
    esp_ble_mesh_cfg_client_set_state_t set = {0};

    if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
        set.app_key_add.net_idx = PRIMARY_NET_IDX;
        set.app_key_add.app_idx = IMAGE_APP_IDX;
        memcpy(set.app_key_add.app_key, s_app_key, sizeof(s_app_key));
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
        set.model_app_bind.element_addr = node->unicast;
        set.model_app_bind.model_app_idx = IMAGE_APP_IDX;
        set.model_app_bind.model_id = BLE_MESH_IMAGE_SOURCE_MODEL_ID;
        set.model_app_bind.company_id = BLE_MESH_IMAGE_COMPANY_ID;
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET) {
        set.model_pub_set.element_addr = node->unicast;
        set.model_pub_set.publish_addr = SERVER_UNICAST_ADDR;
        set.model_pub_set.publish_app_idx = IMAGE_APP_IDX;
        set.model_pub_set.cred_flag = false;
        set.model_pub_set.publish_ttl = CONTROL_TTL;
        set.model_pub_set.publish_period = 0U;
        set.model_pub_set.publish_retransmit =
            ESP_BLE_MESH_PUBLISH_TRANSMIT(0, 50);
        set.model_pub_set.model_id = BLE_MESH_IMAGE_SOURCE_MODEL_ID;
        set.model_pub_set.company_id = BLE_MESH_IMAGE_COMPANY_ID;
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_RELAY_SET) {
        set.relay_set.relay = ESP_BLE_MESH_RELAY_ENABLED;
        set.relay_set.relay_retransmit = ESP_BLE_MESH_TRANSMIT(0, 20);
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_NETWORK_TRANSMIT_SET) {
        set.net_transmit_set.net_transmit = ESP_BLE_MESH_TRANSMIT(0, 20);
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_DEFAULT_TTL_SET) {
        set.default_ttl_set.ttl = CONTROL_TTL;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_ble_mesh_config_client_set_state(&common, &set);
    if (err == ESP_OK) {
        node->pending_opcode = opcode;
    }
    return err;
}

static esp_err_t load_or_create_app_key(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KEY_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = sizeof(s_app_key);
    err = nvs_get_blob(handle, KEY_NVS_APP_KEY, s_app_key, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND || size != sizeof(s_app_key)) {
        esp_fill_random(s_app_key, sizeof(s_app_key));
        err = nvs_set_blob(handle, KEY_NVS_APP_KEY,
                           s_app_key, sizeof(s_app_key));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
    }
    nvs_close(handle);
    return err;
}

static esp_err_t enable_provisioning(void)
{
    if (s_provisioning_enabled) {
        return ESP_OK;
    }
    resume_node_configuration();
    esp_err_t err = esp_ble_mesh_provisioner_prov_enable(
        (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV |
                                     ESP_BLE_MESH_PROV_GATT));
    if (err == ESP_OK) {
        s_provisioning_enabled = true;
        ESP_LOGI(TAG, "auto provisioning enabled for UUID prefix 32 10");
    }
    return err;
}

static esp_err_t bind_local_gateway_model(void)
{
    return esp_ble_mesh_provisioner_bind_app_key_to_local_model(
        SERVER_UNICAST_ADDR, IMAGE_APP_IDX,
        BLE_MESH_IMAGE_GATEWAY_MODEL_ID, BLE_MESH_IMAGE_COMPANY_ID);
}

static bool local_gateway_model_is_bound(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(s_vendor_models[0].keys); ++i) {
        if (s_vendor_models[0].keys[i] == IMAGE_APP_IDX) {
            return true;
        }
    }
    return false;
}

static void provisioning_callback(esp_ble_mesh_prov_cb_event_t event,
                                  esp_ble_mesh_prov_cb_param_t *param)
{
    if (param == NULL) {
        return;
    }

    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
        const uint8_t *uuid =
            param->provisioner_recv_unprov_adv_pkt.dev_uuid;
        if (uuid[0] != 0x32U || uuid[1] != 0x10U) {
            break;
        }
        esp_ble_mesh_unprov_dev_add_t device = {0};
        memcpy(device.addr, param->provisioner_recv_unprov_adv_pkt.addr,
               BD_ADDR_LEN);
        device.addr_type =
            param->provisioner_recv_unprov_adv_pkt.addr_type;
        memcpy(device.uuid, uuid, 16U);
        device.oob_info =
            param->provisioner_recv_unprov_adv_pkt.oob_info;
        device.bearer = param->provisioner_recv_unprov_adv_pkt.bearer;
        esp_err_t err = esp_ble_mesh_provisioner_add_unprov_dev(
            &device,
            (esp_ble_mesh_dev_add_flag_t)(ADD_DEV_RM_AFTER_PROV_FLAG |
                                          ADD_DEV_START_PROV_NOW_FLAG |
                                          ADD_DEV_FLUSHABLE_DEV_FLAG));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "could not queue C6 for provisioning: %s",
                     esp_err_to_name(err));
        }
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: {
        configured_node_t *node = store_node(
            param->provisioner_prov_complete.device_uuid,
            param->provisioner_prov_complete.unicast_addr,
            param->provisioner_prov_complete.element_num);
        if (node == NULL) {
            ESP_LOGE(TAG, "node table full after provisioning 0x%04x",
                     param->provisioner_prov_complete.unicast_addr);
            break;
        }
        ESP_LOGI(TAG, "C6 provisioned addr=0x%04x elements=%u",
                 node->unicast, node->element_count);
        node->retries = 0U;
        esp_err_t err = send_node_config(
            node, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AppKey Add enqueue failed: %s",
                     esp_err_to_name(err));
        }
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        if (param->provisioner_add_app_key_comp.err_code == ESP_OK) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(bind_local_gateway_model());
        } else {
            ESP_LOGE(TAG, "local AppKey add failed: %d",
                     param->provisioner_add_app_key_comp.err_code);
        }
        break;
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        if (param->provisioner_bind_app_key_to_model_comp.err_code == ESP_OK) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(enable_provisioning());
        } else {
            ESP_LOGE(TAG, "local Gateway model bind failed: %d",
                     param->provisioner_bind_app_key_to_model_comp.err_code);
        }
        break;
    default:
        break;
    }
}

static void config_client_callback(esp_ble_mesh_cfg_client_cb_event_t event,
                                   esp_ble_mesh_cfg_client_cb_param_t *param)
{
    if (param == NULL || param->params == NULL) {
        return;
    }
    configured_node_t *node = find_node(param->params->ctx.addr);
    if (node == NULL) {
        return;
    }

    uint32_t opcode = param->params->opcode;
    bool timed_out = event == ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT;
    if (timed_out || param->error_code != 0) {
        if (node->retries++ < CONFIG_RETRIES) {
            ESP_LOGW(TAG, "retry config opcode=0x%08" PRIx32
                          " addr=0x%04x attempt=%u",
                     opcode, node->unicast, node->retries);
            ESP_ERROR_CHECK_WITHOUT_ABORT(send_node_config(node, opcode));
        } else {
            ESP_LOGE(TAG, "configuration failed addr=0x%04x opcode=0x%08"
                          PRIx32, node->unicast, opcode);
        }
        return;
    }

    if (event != ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT) {
        return;
    }

    if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
        uint8_t status = param->status_cb.appkey_status.status;
        if (status != ESP_BLE_MESH_CFG_STATUS_SUCCESS) {
            /*
             * KEY_INDEX_ALREADY_STORED means that this index can contain a
             * different key. Treating it as success could make both models
             * look READY while all vendor traffic fails authentication. A
             * repeated Add of the same persisted key is idempotent SUCCESS.
             */
            ESP_LOGE(TAG, "C6 AppKey status=%u addr=0x%04x; "
                          "key conflict requires node reset/reprovision",
                     status, node->unicast);
            return;
        }
        node->retries = 0U;
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_node_config(
            node, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND));
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
        if (param->status_cb.model_app_status.status == 0U) {
            node->retries = 0U;
            ESP_ERROR_CHECK_WITHOUT_ABORT(send_node_config(
                node, ESP_BLE_MESH_MODEL_OP_RELAY_SET));
        } else {
            ESP_LOGE(TAG, "C6 model bind status=%u addr=0x%04x",
                     param->status_cb.model_app_status.status,
                     node->unicast);
        }
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET) {
        if (param->status_cb.model_pub_status.status == 0U) {
            node->pending_opcode = 0U;
            node->retries = 0U;
            ESP_LOGI(TAG, "C6 image model ready addr=0x%04x ttl=%u",
                     node->unicast, CONTROL_TTL);
        } else {
            ESP_LOGE(TAG, "C6 publication status=%u addr=0x%04x",
                     param->status_cb.model_pub_status.status,
                     node->unicast);
        }
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_RELAY_SET) {
        node->retries = 0U;
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_node_config(
            node, ESP_BLE_MESH_MODEL_OP_NETWORK_TRANSMIT_SET));
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_NETWORK_TRANSMIT_SET) {
        node->retries = 0U;
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_node_config(
            node, ESP_BLE_MESH_MODEL_OP_DEFAULT_TTL_SET));
    } else if (opcode == ESP_BLE_MESH_MODEL_OP_DEFAULT_TTL_SET) {
        node->retries = 0U;
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_node_config(
            node, ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET));
    }
}

static uint8_t raw_image_opcode(uint32_t opcode)
{
    if (opcode == IMAGE_OPCODE(BLE_MESH_IMAGE_OP_OPEN)) {
        return BLE_MESH_IMAGE_OP_OPEN;
    }
    if (opcode == IMAGE_OPCODE(BLE_MESH_IMAGE_OP_DATA)) {
        return BLE_MESH_IMAGE_OP_DATA;
    }
    if (opcode == IMAGE_OPCODE(BLE_MESH_IMAGE_OP_END)) {
        return BLE_MESH_IMAGE_OP_END;
    }
    return 0U;
}

static esp_err_t send_reply(const image_reassembly_reply_t *reply)
{
    esp_ble_mesh_msg_ctx_t context = {
        .net_idx = PRIMARY_NET_IDX,
        .app_idx = IMAGE_APP_IDX,
        .addr = reply->destination,
        .send_ttl = CONTROL_TTL,
    };
    return esp_ble_mesh_client_model_send_msg(
        &s_vendor_models[0], &context, IMAGE_OPCODE(reply->opcode),
        (uint16_t)reply->payload_len, (uint8_t *)reply->payload,
        0, false, ROLE_PROVISIONER);
}

static bool sample_time_provider(uint64_t *unix_ms)
{
    mesh_image_gateway_time_provider_t provider;
    void *provider_ctx;

    portENTER_CRITICAL(&s_callback_lock);
    provider = s_time_provider;
    provider_ctx = s_time_provider_ctx;
    portEXIT_CRITICAL(&s_callback_lock);
    return provider != NULL && provider(unix_ms, provider_ctx);
}

static void publish_complete(const image_reassembly_complete_t *complete)
{
    mesh_image_gateway_image_cb_t callback;
    void *callback_ctx;

    portENTER_CRITICAL(&s_callback_lock);
    callback = s_image_callback;
    callback_ctx = s_image_callback_ctx;
    portEXIT_CRITICAL(&s_callback_lock);

    mesh_image_gateway_image_t image = {
        .source_addr = complete->source_addr,
        .event_time_ms = complete->detected_at_ms,
        .time_source = complete->detected_at_ms != 0U ?
                       SERVER_TIME_P4_DETECTED :
                       SERVER_TIME_UNKNOWN,
        .jpeg = complete->jpeg,
        .jpeg_len = complete->jpeg_len,
    };
    if (image.time_source == SERVER_TIME_UNKNOWN &&
        complete->rx_estimate_ms != 0U) {
        image.event_time_ms = complete->rx_estimate_ms;
        image.time_source = SERVER_TIME_RX_ESTIMATE;
    }

    if (callback != NULL) {
        callback(&image, callback_ctx);
    }
}

static void custom_model_callback(esp_ble_mesh_model_cb_event_t event,
                                  esp_ble_mesh_model_cb_param_t *param)
{
    if (param == NULL) {
        return;
    }

    uint32_t received_opcode;
    esp_ble_mesh_model_t *received_model;
    esp_ble_mesh_msg_ctx_t *context;
    uint16_t received_length;
    uint8_t *received_message;
    if (event == ESP_BLE_MESH_CLIENT_MODEL_RECV_PUBLISH_MSG_EVT) {
        received_opcode = param->client_recv_publish_msg.opcode;
        received_model = param->client_recv_publish_msg.model;
        context = param->client_recv_publish_msg.ctx;
        received_length = param->client_recv_publish_msg.length;
        received_message = param->client_recv_publish_msg.msg;
    } else if (event == ESP_BLE_MESH_MODEL_OPERATION_EVT) {
        /* Also handle a response matched by the client common layer. */
        received_opcode = param->model_operation.opcode;
        received_model = param->model_operation.model;
        context = param->model_operation.ctx;
        received_length = param->model_operation.length;
        received_message = param->model_operation.msg;
    } else {
        return;
    }
    if (received_model != &s_vendor_models[0] || context == NULL) {
        return;
    }

    uint8_t opcode = raw_image_opcode(received_opcode);
    if (opcode == 0U || context->net_idx != PRIMARY_NET_IDX ||
        context->app_idx != IMAGE_APP_IDX ||
        !ESP_BLE_MESH_ADDR_IS_UNICAST(context->addr) ||
        esp_ble_mesh_provisioner_get_node_with_addr(context->addr) == NULL) {
        return;
    }

    image_reassembly_reply_t replies[IMAGE_REASSEMBLY_REPLY_MAX] = {0};
    image_reassembly_complete_t complete = {0};
    uint64_t open_rx_estimate_ms = 0U;
    if (opcode == BLE_MESH_IMAGE_OP_OPEN &&
        received_message != NULL &&
        received_length == sizeof(ble_mesh_image_open_t) &&
        ble_mesh_image_get_le64(received_message + 2U) == 0U) {
        (void)sample_time_provider(&open_rx_estimate_ms);
    }
    xSemaphoreTake(s_reassembly_mutex, portMAX_DELAY);
    bool idle_work_busy = s_idle_work_reserved;
    bool injected_drop = false;
#if CONFIG_SERVER_TEST_DROP_CHUNK_INDEX >= 0
    if (!idle_work_busy && !s_test_drop_done &&
        opcode == BLE_MESH_IMAGE_OP_DATA &&
        received_message != NULL &&
        received_length >
            sizeof(ble_mesh_image_data_header_t) &&
        s_reassembly.active && s_reassembly.source_addr == context->addr &&
        ble_mesh_image_get_le16(received_message) ==
            s_reassembly.frame_id &&
        received_message[2] ==
            (uint8_t)CONFIG_SERVER_TEST_DROP_CHUNK_INDEX) {
        s_test_drop_done = true;
        injected_drop = true;
    }
#endif
    size_t reply_count = 0U;
    if (idle_work_busy) {
        uint16_t frame_id = received_message != NULL &&
                            received_length >= 2U ?
                            ble_mesh_image_get_le16(received_message) : 0U;
        replies[0].opcode = opcode == BLE_MESH_IMAGE_OP_OPEN ?
                            BLE_MESH_IMAGE_OP_BUSY :
                            BLE_MESH_IMAGE_OP_RESTART;
        replies[0].destination = context->addr;
        replies[0].payload_len = sizeof(ble_mesh_image_frame_t);
        ble_mesh_image_put_le16(replies[0].payload, frame_id);
        reply_count = 1U;
    } else if (!injected_drop) {
        reply_count = image_reassembly_receive(
            &s_reassembly, context->addr, opcode,
            received_message, received_length,
            monotonic_ms(), replies, ARRAY_SIZE(replies), &complete);
        if (opcode == BLE_MESH_IMAGE_OP_OPEN && reply_count == 1U &&
            replies[0].opcode == BLE_MESH_IMAGE_OP_ACCEPT &&
            received_message != NULL && received_length >= 2U) {
            image_reassembly_set_rx_estimate(
                &s_reassembly, context->addr,
                ble_mesh_image_get_le16(received_message),
                open_rx_estimate_ms);
        }
    }
    xSemaphoreGive(s_reassembly_mutex);

    if (injected_drop) {
        ESP_LOGW(TAG, "TEST dropped DATA src=0x%04x chunk=%d",
                 context->addr, CONFIG_SERVER_TEST_DROP_CHUNK_INDEX);
        return;
    }

    for (size_t i = 0; i < reply_count; ++i) {
        if (replies[i].opcode == BLE_MESH_IMAGE_OP_NACK) {
            uint8_t bitmap[BLE_MESH_IMAGE_NACK_BITMAP_MAX] = {0};
            size_t bitmap_len = replies[i].payload_len -
                                sizeof(ble_mesh_image_nack_header_t);
            memcpy(bitmap,
                   replies[i].payload + sizeof(ble_mesh_image_nack_header_t),
                   bitmap_len);
            ESP_LOGW(TAG, "NACK src=0x%04x frame=%u base=%u "
                          "bitmap=%02x%02x%02x%02x%02x bytes=%u",
                     replies[i].destination,
                     ble_mesh_image_get_le16(replies[i].payload),
                     replies[i].payload[2],
                     bitmap[0], bitmap[1], bitmap[2], bitmap[3], bitmap[4],
                     (unsigned)bitmap_len);
        } else if (replies[i].opcode == BLE_MESH_IMAGE_OP_REJECT &&
                   replies[i].payload_len == sizeof(ble_mesh_image_reject_t)) {
            ESP_LOGW(TAG, "REJECT src=0x%04x frame=%u reason=%u",
                     replies[i].destination,
                     ble_mesh_image_get_le16(replies[i].payload),
                     replies[i].payload[2]);
        }
        esp_err_t err = send_reply(&replies[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "reply C%X to 0x%04x failed: %s",
                     replies[i].opcode & 0x0FU, replies[i].destination,
                     esp_err_to_name(err));
        }
    }
    if (complete.valid) {
        ESP_LOGI(TAG,
                 "frame complete src=0x%04x frame=%u bytes=%u "
                 "crc=ok jpeg=224x224 detected_at_ms=%" PRIu64,
                 complete.source_addr, complete.frame_id,
                 (unsigned)complete.jpeg_len, complete.detected_at_ms);
        publish_complete(&complete);
    }
}

static void timeout_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TIMEOUT_POLL_MS));
        image_reassembly_reply_t reply = {0};
        xSemaphoreTake(s_reassembly_mutex, portMAX_DELAY);
        size_t count = image_reassembly_poll_timeout(
            &s_reassembly, monotonic_ms(), &reply);
        xSemaphoreGive(s_reassembly_mutex);
        if (count != 0U) {
            ESP_LOGW(TAG, "frame timeout src=0x%04x", reply.destination);
            ESP_ERROR_CHECK_WITHOUT_ABORT(send_reply(&reply));
        }
    }
}

#if defined(CONFIG_BT_NIMBLE_ENABLED)
static void nimble_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset reason=%d", reason);
}

static void nimble_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_bt_addr_type);
    }
    if (rc == 0) {
        rc = ble_hs_id_copy_addr(s_bt_addr_type, s_bt_addr, NULL);
    }
    s_bt_sync_result = rc == 0 ? ESP_OK : ESP_FAIL;
    xSemaphoreGive(s_bt_sync_sem);
}

static void nimble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t bluetooth_init(void)
{
    s_bt_sync_sem = xSemaphoreCreateBinary();
    if (s_bt_sync_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_hs_cfg.reset_cb = nimble_reset;
    ble_hs_cfg.sync_cb = nimble_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    nimble_port_freertos_init(nimble_host_task);
    if (xSemaphoreTake(s_bt_sync_sem,
                       pdMS_TO_TICKS(BT_SYNC_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_bt_sync_result == ESP_OK) {
        memcpy(s_prov_uuid + 2U, s_bt_addr, sizeof(s_bt_addr));
    }
    return s_bt_sync_result;
}
#elif defined(CONFIG_BT_BLUEDROID_ENABLED)
static esp_err_t bluetooth_init(void)
{
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (err == ESP_OK) {
        err = esp_bt_controller_init(&config);
    }
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
        memcpy(s_prov_uuid + 2U, esp_bt_dev_get_address(), 6U);
    }
    return err;
}
#else
static esp_err_t bluetooth_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

esp_err_t mesh_image_gateway_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_reassembly_mutex = xSemaphoreCreateMutex();
    if (s_reassembly_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    image_reassembly_init(&s_reassembly);
    s_idle_work_reserved = false;

    esp_err_t err = bluetooth_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_mesh_register_prov_callback(provisioning_callback);
    if (err == ESP_OK) {
        err = esp_ble_mesh_register_config_client_callback(
            config_client_callback);
    }
    if (err == ESP_OK) {
        err = esp_ble_mesh_register_custom_model_callback(
            custom_model_callback);
    }
    if (err == ESP_OK) {
        err = esp_ble_mesh_init(&s_provision, &s_composition);
    }
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ble_mesh_client_model_init(&s_vendor_models[0]);
    if (err != ESP_OK) {
        return err;
    }
    restore_provisioned_nodes();

    const uint8_t uuid_match[2] = {0x32U, 0x10U};
    err = esp_ble_mesh_provisioner_set_dev_uuid_match(
        uuid_match, sizeof(uuid_match), 0U, false);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t *existing = esp_ble_mesh_provisioner_get_local_app_key(
        PRIMARY_NET_IDX, IMAGE_APP_IDX);
    if (existing != NULL) {
        memcpy(s_app_key, existing, sizeof(s_app_key));
        err = local_gateway_model_is_bound() ?
              enable_provisioning() : bind_local_gateway_model();
    } else {
        err = load_or_create_app_key();
        if (err == ESP_OK) {
            err = esp_ble_mesh_provisioner_add_local_app_key(
                s_app_key, PRIMARY_NET_IDX, IMAGE_APP_IDX);
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(timeout_task, "image_timeout", 3072U, NULL, 4U, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "board-neutral BLE Mesh image Provisioner initialized");
    return ESP_OK;
}
