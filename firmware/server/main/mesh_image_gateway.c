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
#define DEVICE_UUID_ID_OFFSET     8U
#define DEVICE_UUID_BT_ADDR_OFFSET 2U
#define DEVICE_UUID_BT_ADDR_BYTES 6U
#define DEVICE_UUID_RESERVED_OFFSET 10U
#define DEVICE_ID_MIN             UINT16_C(1)
#define DEVICE_ID_MAX             UINT16_C(0x7FFE)
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
    uint16_t device_id;
    uint16_t unicast;
    uint8_t element_count;
    uint32_t pending_opcode;
    uint8_t retries;
} configured_node_t;

static uint8_t s_prov_uuid[16] = {0x47U, 0x57U};
static uint8_t s_app_key[16];
static bool s_gateway_ready;
static bool s_provisioning_active;
static uint16_t s_pending_device_id;
static uint8_t s_pending_device_uuid[16];
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
    ESP_BLE_MESH_MODEL_OP(IMAGE_OPCODE(BLE_MESH_IMAGE_OP_TIME_REQUEST),
                          sizeof(ble_mesh_time_request_t)),
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
_Static_assert(CONFIG_SERVER_MAX_NODES <= CONFIG_BLE_MESH_MAX_PROV_NODES,
               "server runtime node capacity exceeds Mesh node capacity");
static image_reassembly_t s_reassembly;
static SemaphoreHandle_t s_reassembly_mutex;
static bool s_idle_work_reserved;

static portMUX_TYPE s_callback_lock = portMUX_INITIALIZER_UNLOCKED;
static mesh_image_gateway_image_cb_t s_image_callback;
static void *s_image_callback_ctx;
static mesh_image_gateway_time_provider_t s_time_provider;
static void *s_time_provider_ctx;

static esp_err_t send_node_config(configured_node_t *node, uint32_t opcode);

static uint16_t device_id_from_uuid(const uint8_t uuid[16])
{
    if (uuid == NULL || uuid[0] != 0x32U || uuid[1] != 0x10U) {
        return 0U;
    }
    return (uint16_t)uuid[DEVICE_UUID_ID_OFFSET] |
           ((uint16_t)uuid[DEVICE_UUID_ID_OFFSET + 1U] << 8);
}

static bool device_uuid_layout_valid(const uint8_t uuid[16])
{
    if (uuid == NULL) {
        return false;
    }
    const uint16_t device_id = device_id_from_uuid(uuid);
    if (device_id < DEVICE_ID_MIN || device_id > DEVICE_ID_MAX) {
        return false;
    }

    bool address_is_nonzero = false;
    for (size_t i = 0; i < DEVICE_UUID_BT_ADDR_BYTES; ++i) {
        address_is_nonzero |= uuid[DEVICE_UUID_BT_ADDR_OFFSET + i] != 0U;
    }
    if (!address_is_nonzero) {
        return false;
    }

    for (size_t i = DEVICE_UUID_RESERVED_OFFSET; i < 16U; ++i) {
        if (uuid[i] != 0U) {
            return false;
        }
    }
    return true;
}

static bool device_uuid_has_same_bt_address(const uint8_t left[16],
                                            const uint8_t right[16])
{
    return left != NULL && right != NULL &&
           memcmp(left + DEVICE_UUID_BT_ADDR_OFFSET,
                  right + DEVICE_UUID_BT_ADDR_OFFSET,
                  DEVICE_UUID_BT_ADDR_BYTES) == 0;
}

static uint16_t device_addr_from_id(uint16_t device_id)
{
    return device_id >= DEVICE_ID_MIN && device_id <= DEVICE_ID_MAX ?
           (uint16_t)(SERVER_UNICAST_ADDR + device_id) : 0U;
}

uint16_t mesh_image_gateway_device_id_from_addr(uint16_t source_addr)
{
    if (source_addr <= SERVER_UNICAST_ADDR || source_addr > 0x7FFFU) {
        return 0U;
    }
    return (uint16_t)(source_addr - SERVER_UNICAST_ADDR);
}

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

static configured_node_t *find_node_by_uuid(const uint8_t uuid[16])
{
    if (uuid == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ARRAY_SIZE(s_nodes); ++i) {
        if (s_nodes[i].used && memcmp(s_nodes[i].uuid, uuid, 16U) == 0) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

static configured_node_t *find_node_by_bt_address(const uint8_t uuid[16])
{
    if (uuid == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ARRAY_SIZE(s_nodes); ++i) {
        if (s_nodes[i].used &&
            device_uuid_has_same_bt_address(s_nodes[i].uuid, uuid)) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

static const esp_ble_mesh_node_t *find_mesh_node_by_bt_address(
    const uint8_t uuid[16])
{
    const esp_ble_mesh_node_t **table =
        esp_ble_mesh_provisioner_get_node_table_entry();
    if (uuid == NULL || table == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < CONFIG_BLE_MESH_MAX_PROV_NODES; ++i) {
        if (table[i] != NULL &&
            device_uuid_has_same_bt_address(table[i]->dev_uuid, uuid)) {
            return table[i];
        }
    }
    return NULL;
}

static const esp_ble_mesh_node_t *find_mesh_node_by_device_id(
    uint16_t device_id)
{
    const esp_ble_mesh_node_t **table =
        esp_ble_mesh_provisioner_get_node_table_entry();
    if (table == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < CONFIG_BLE_MESH_MAX_PROV_NODES; ++i) {
        if (table[i] != NULL &&
            device_id_from_uuid(table[i]->dev_uuid) == device_id) {
            return table[i];
        }
    }
    return NULL;
}

static bool has_free_runtime_node_slot(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(s_nodes); ++i) {
        if (!s_nodes[i].used) {
            return true;
        }
    }
    return false;
}

static configured_node_t *store_node(const uint8_t uuid[16],
                                     uint16_t unicast,
                                     uint8_t element_count)
{
    const uint16_t device_id = device_id_from_uuid(uuid);
    const uint16_t expected_addr = device_addr_from_id(device_id);
    if (!device_uuid_layout_valid(uuid) || expected_addr == 0U ||
        unicast != expected_addr ||
        element_count != 1U) {
        ESP_LOGE(TAG,
                 "invalid C6 identity id=%u addr=0x%04x expected=0x%04x "
                 "elements=%u; erase server Mesh NVS and reset every "
                 "registered C6 before reprovisioning",
                 device_id, unicast, expected_addr, element_count);
        return NULL;
    }

    configured_node_t *entry = find_node(unicast);
    configured_node_t *uuid_entry = find_node_by_uuid(uuid);
    configured_node_t *bt_entry = find_node_by_bt_address(uuid);
    if ((entry != NULL && memcmp(entry->uuid, uuid, 16U) != 0) ||
        (uuid_entry != NULL && uuid_entry->unicast != unicast) ||
        (bt_entry != NULL && bt_entry->device_id != device_id)) {
        ESP_LOGE(TAG,
                 "C6 ID/address collision id=%u addr=0x%04x; "
                 "each device must use a unique C6_DEVICE_ID",
                 device_id, unicast);
        return NULL;
    }
    if (entry == NULL) {
        entry = uuid_entry;
    }
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
        .device_id = device_id,
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
            ESP_LOGE(TAG,
                     "could not restore C6 identity at 0x%04x; "
                     "erase server Mesh NVS and reset every registered C6",
                     stored->unicast_addr);
            continue;
        }
        ESP_LOGI(TAG,
                 "restored C6 id=%u addr=0x%04x for idempotent config",
                 node->device_id, node->unicast);
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

static void mark_gateway_ready(void)
{
    if (s_gateway_ready) {
        return;
    }

    s_gateway_ready = true;
    resume_node_configuration();
    ESP_LOGI(TAG, "Gateway AppKey ready; auto provisioning active for "
                  "UUID prefix 32 10");
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
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT: {
        if (param->provisioner_prov_enable_comp.err_code != ESP_OK) {
            ESP_LOGE(TAG, "Provisioner enable failed: %d",
                     param->provisioner_prov_enable_comp.err_code);
            break;
        }

        /* ESP-IDF creates or restores the Provisioner's Primary NetKey as
         * part of PROV_ENABLE.  Local AppKey operations before this event
         * fail asynchronously with -ENODEV (Invalid NetKeyIndex 0x0000).
         * Keep all local AppKey setup behind this completion event. */
        if (local_gateway_model_is_bound()) {
            const uint8_t *existing =
                esp_ble_mesh_provisioner_get_local_app_key(
                    PRIMARY_NET_IDX, IMAGE_APP_IDX);
            if (existing == NULL) {
                ESP_LOGE(TAG, "Gateway model is bound but AppKey is missing; "
                              "erase the server Mesh NVS and reset all nodes");
                break;
            }
            memcpy(s_app_key, existing, sizeof(s_app_key));
            mark_gateway_ready();
            break;
        }

        esp_err_t err = load_or_create_app_key();
        if (err == ESP_OK) {
            /* This is idempotent when an earlier boot stored the same key but
             * stopped before binding the local model. */
            err = esp_ble_mesh_provisioner_add_local_app_key(
                s_app_key, PRIMARY_NET_IDX, IMAGE_APP_IDX);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "local AppKey setup enqueue failed: %s",
                     esp_err_to_name(err));
        }
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
        /* Provisioner scanning starts before local AppKey add/bind completes.
         * A device keeps advertising, so ignoring it during this short window
         * is safe and prevents provisioning a node we cannot configure. */
        if (!s_gateway_ready) {
            break;
        }
        const uint8_t *uuid =
            param->provisioner_recv_unprov_adv_pkt.dev_uuid;
        if (uuid[0] != 0x32U || uuid[1] != 0x10U) {
            break;
        }
        const uint16_t device_id = device_id_from_uuid(uuid);
        const uint16_t target_addr = device_addr_from_id(device_id);
        if (!device_uuid_layout_valid(uuid) || target_addr == 0U) {
            ESP_LOGE(TAG,
                     "reject C6 with invalid UUID layout/device ID %u; "
                     "set C6_DEVICE_ID to 1..%u and use current firmware",
                     device_id, DEVICE_ID_MAX);
            break;
        }

        esp_ble_mesh_node_t *mesh_address_owner =
            esp_ble_mesh_provisioner_get_node_with_addr(target_addr);
        const esp_ble_mesh_node_t *mesh_bt_owner =
            find_mesh_node_by_bt_address(uuid);
        const esp_ble_mesh_node_t *mesh_id_owner =
            find_mesh_node_by_device_id(device_id);
        configured_node_t *address_owner = find_node(target_addr);
        configured_node_t *uuid_owner = find_node_by_uuid(uuid);
        configured_node_t *bt_owner = find_node_by_bt_address(uuid);
        if (mesh_address_owner != NULL || mesh_bt_owner != NULL ||
            mesh_id_owner != NULL || address_owner != NULL ||
            uuid_owner != NULL || bt_owner != NULL) {
            if (address_owner != NULL &&
                memcmp(address_owner->uuid, uuid, 16U) != 0) {
                ESP_LOGE(TAG,
                         "reject duplicate C6 device ID %u: addr=0x%04x "
                         "already belongs to another UUID",
                         device_id, target_addr);
            } else if (mesh_address_owner != NULL &&
                       memcmp(mesh_address_owner->dev_uuid, uuid, 16U) != 0) {
                ESP_LOGE(TAG,
                         "reject C6 id=%u: Mesh address 0x%04x overlaps a "
                         "persisted node; assign this new C6 an unused "
                         "C6_DEVICE_ID (replacing the old node requires a "
                         "full Mesh reset)",
                         device_id, target_addr);
            } else if (bt_owner != NULL &&
                       bt_owner->device_id != device_id) {
                ESP_LOGE(TAG,
                         "reject C6 Bluetooth identity already registered as "
                         "id=%u addr=0x%04x; erase server Mesh NVS and reset "
                         "every registered C6 before changing C6_DEVICE_ID",
                         bt_owner->device_id, bt_owner->unicast);
            } else if (mesh_bt_owner != NULL &&
                       memcmp(mesh_bt_owner->dev_uuid, uuid, 16U) != 0) {
                ESP_LOGE(TAG,
                         "reject C6 Bluetooth identity retained in Mesh NVS "
                         "at 0x%04x; erase server Mesh NVS and reset every "
                         "registered C6 before changing C6_DEVICE_ID",
                         mesh_bt_owner->unicast_addr);
            } else if (mesh_bt_owner != NULL &&
                       mesh_bt_owner->unicast_addr != target_addr) {
                ESP_LOGE(TAG,
                         "reject C6 UUID retained at stale addr=0x%04x "
                         "instead of id=%u addr=0x%04x; erase server Mesh "
                         "NVS and reset every registered C6",
                         mesh_bt_owner->unicast_addr, device_id, target_addr);
            } else if (mesh_id_owner != NULL &&
                       memcmp(mesh_id_owner->dev_uuid, uuid, 16U) != 0) {
                ESP_LOGE(TAG,
                         "reject duplicate C6 device ID %u retained at "
                         "Mesh addr=0x%04x; IDs must be unique",
                         device_id, mesh_id_owner->unicast_addr);
            } else {
                ESP_LOGW(TAG,
                         "C6 id=%u addr=0x%04x is already in server NVS; "
                         "erase server Mesh NVS and reset all C6 nodes before "
                         "reprovisioning",
                         device_id, target_addr);
            }
            break;
        }
        if (s_provisioning_active) {
            /* Only one provisioning link can be active. Other devices keep
             * advertising and will be handled after this link closes. */
            break;
        }
        if (!has_free_runtime_node_slot()) {
            ESP_LOGE(TAG,
                     "reject C6 id=%u: runtime node capacity %u is full",
                     device_id, (unsigned)ARRAY_SIZE(s_nodes));
            break;
        }

        esp_err_t err = esp_ble_mesh_provisioner_prov_device_with_addr(
            uuid, param->provisioner_recv_unprov_adv_pkt.addr,
            param->provisioner_recv_unprov_adv_pkt.addr_type,
            param->provisioner_recv_unprov_adv_pkt.bearer,
            param->provisioner_recv_unprov_adv_pkt.oob_info, target_addr);
        if (err == ESP_OK) {
            s_provisioning_active = true;
            s_pending_device_id = device_id;
            memcpy(s_pending_device_uuid, uuid,
                   sizeof(s_pending_device_uuid));
            ESP_LOGI(TAG,
                     "provisioning C6 id=%u with fixed addr=0x%04x",
                     device_id, target_addr);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "could not start C6 id=%u provisioning: %s",
                     device_id, esp_err_to_name(err));
        }
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_PROV_DEV_WITH_ADDR_COMP_EVT:
        if (param->provisioner_prov_dev_with_addr_comp.err_code != ESP_OK) {
            ESP_LOGE(TAG,
                     "fixed-address provisioning start failed id=%u err=%d",
                     s_pending_device_id,
                     param->provisioner_prov_dev_with_addr_comp.err_code);
            s_provisioning_active = false;
            s_pending_device_id = 0U;
            memset(s_pending_device_uuid, 0, sizeof(s_pending_device_uuid));
        }
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT:
        if (s_provisioning_active) {
            ESP_LOGI(TAG,
                     "provisioning link closed id=%u reason=0x%02x",
                     s_pending_device_id,
                     param->provisioner_prov_link_close.reason);
        }
        s_provisioning_active = false;
        s_pending_device_id = 0U;
        memset(s_pending_device_uuid, 0, sizeof(s_pending_device_uuid));
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: {
        /* Keep the attempt reserved until PROV_LINK_CLOSE.  ESP-IDF reports
         * COMPLETE before closing the link; clearing here could let an older
         * CLOSE event cancel a newly-started device. */
        const uint8_t *completed_uuid =
            param->provisioner_prov_complete.device_uuid;
        const uint16_t completed_id = device_id_from_uuid(completed_uuid);
        if (!s_provisioning_active || completed_id != s_pending_device_id ||
            memcmp(completed_uuid, s_pending_device_uuid,
                   sizeof(s_pending_device_uuid)) != 0) {
            ESP_LOGE(TAG,
                     "ignore mismatched provisioning completion id=%u "
                     "pending=%u active=%u",
                     completed_id, s_pending_device_id,
                     s_provisioning_active ? 1U : 0U);
            break;
        }
        configured_node_t *node = store_node(
            completed_uuid,
            param->provisioner_prov_complete.unicast_addr,
            param->provisioner_prov_complete.element_num);
        if (node == NULL) {
            ESP_LOGE(TAG,
                     "provisioned node identity/address validation failed "
                     "at 0x%04x; erase server Mesh NVS and reset every "
                     "registered C6 before retrying",
                     param->provisioner_prov_complete.unicast_addr);
            break;
        }
        ESP_LOGI(TAG, "C6 provisioned id=%u addr=0x%04x elements=%u",
                 node->device_id, node->unicast, node->element_count);
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
            mark_gateway_ready();
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
            ESP_LOGE(TAG, "C6 AppKey status=%u addr=0x%04x; erase server "
                          "Mesh NVS and reset every registered C6 before "
                          "reprovisioning",
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
            ESP_LOGI(TAG,
                     "C6 image model ready id=%u addr=0x%04x ttl=%u",
                     node->device_id, node->unicast, CONTROL_TTL);
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
    if (opcode == IMAGE_OPCODE(BLE_MESH_IMAGE_OP_TIME_REQUEST)) {
        return BLE_MESH_IMAGE_OP_TIME_REQUEST;
    }
    return 0U;
}

static esp_err_t send_vendor_message(uint16_t destination, uint8_t opcode,
                                     const uint8_t *payload,
                                     size_t payload_len)
{
    esp_ble_mesh_msg_ctx_t context = {
        .net_idx = PRIMARY_NET_IDX,
        .app_idx = IMAGE_APP_IDX,
        .addr = destination,
        .send_ttl = CONTROL_TTL,
    };
    return esp_ble_mesh_client_model_send_msg(
        &s_vendor_models[0], &context, IMAGE_OPCODE(opcode),
        (uint16_t)payload_len, (uint8_t *)payload,
        0, false, ROLE_PROVISIONER);
}

static esp_err_t send_reply(const image_reassembly_reply_t *reply)
{
    return send_vendor_message(reply->destination, reply->opcode,
                               reply->payload, reply->payload_len);
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

static void handle_time_request(const esp_ble_mesh_msg_ctx_t *context,
                                const uint8_t *message, size_t message_len)
{
    if (context == NULL || message == NULL ||
        message_len != sizeof(ble_mesh_time_request_t)) {
        ESP_LOGW(TAG, "ignored malformed TIME_REQUEST src=0x%04x bytes=%u",
                 context == NULL ? 0U : context->addr,
                 (unsigned)message_len);
        return;
    }

    uint32_t request_id = ble_mesh_image_get_le32(message);
    uint64_t server_rx_unix_ms = 0U;
    bool rx_valid = sample_time_provider(&server_rx_unix_ms);

    uint8_t status[sizeof(ble_mesh_time_status_message_t)] = {0};
    ble_mesh_image_put_le32(status, request_id);
    status[4] = BLE_MESH_TIME_STATUS_UNAVAILABLE;

    uint64_t server_tx_unix_ms = 0U;
    if (rx_valid && sample_time_provider(&server_tx_unix_ms) &&
        server_tx_unix_ms >= server_rx_unix_ms) {
        status[4] = BLE_MESH_TIME_STATUS_OK;
        ble_mesh_image_put_le64(status + 8U, server_rx_unix_ms);
        ble_mesh_image_put_le64(status + 16U, server_tx_unix_ms);
    } else {
        server_rx_unix_ms = 0U;
        server_tx_unix_ms = 0U;
    }

    esp_err_t err = send_vendor_message(
        context->addr, BLE_MESH_IMAGE_OP_TIME_STATUS,
        status, sizeof(status));
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "TIME_STATUS src=0x%04x request=%lu status=%u rx_ms=%llu "
                 "tx_ms=%llu",
                 context->addr, (unsigned long)request_id, status[4],
                 (unsigned long long)server_rx_unix_ms,
                 (unsigned long long)server_tx_unix_ms);
    } else {
        ESP_LOGW(TAG, "TIME_STATUS src=0x%04x request=%lu failed: %s",
                 context->addr, (unsigned long)request_id,
                 esp_err_to_name(err));
    }
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
        .device_id =
            mesh_image_gateway_device_id_from_addr(complete->source_addr),
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
    configured_node_t *source_node = find_node(context->addr);
    if (opcode == 0U || context->net_idx != PRIMARY_NET_IDX ||
        context->app_idx != IMAGE_APP_IDX ||
        !ESP_BLE_MESH_ADDR_IS_UNICAST(context->addr) ||
        source_node == NULL ||
        device_addr_from_id(source_node->device_id) != context->addr) {
        return;
    }

    if (opcode == BLE_MESH_IMAGE_OP_TIME_REQUEST) {
        handle_time_request(context, received_message, received_length);
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
                 "frame complete id=%u src=0x%04x frame=%u bytes=%u "
                 "crc=ok jpeg=224x224 detected_at_ms=%" PRIu64,
                 mesh_image_gateway_device_id_from_addr(complete.source_addr),
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

    /* PROV_ENABLE creates/restores the primary network.  The completion
     * callback performs AppKey add/bind only after NetKey 0 is usable. */
    err = esp_ble_mesh_provisioner_prov_enable(
        (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV |
                                     ESP_BLE_MESH_PROV_GATT));
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
