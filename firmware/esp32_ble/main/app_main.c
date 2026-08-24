#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"

#include "ble_mesh_image_protocol.h"
#include "device_identity.h"

#define TAG "esp32_ble"

#define DEVICE_ID_MIN                 1U
#define DEVICE_ID_MAX                 32766U
#define DEVICE_UUID_PREFIX_0          0x32U
#define DEVICE_UUID_PREFIX_1          0x10U
#define DEVICE_UUID_BT_ADDR_OFFSET    2U
#define DEVICE_UUID_BT_ADDR_BYTES     6U
#define DEVICE_UUID_ID_OFFSET         8U
#define DEVICE_UUID_RESERVED_OFFSET   10U
#define EXPECTED_UNICAST_ADDR         ((uint16_t)(ESP32_BLE_DEVICE_ID + 1U))

#define IMAGE_OPCODE(raw_opcode) \
    ESP_BLE_MESH_MODEL_OP_3((raw_opcode), BLE_MESH_IMAGE_COMPANY_ID)

_Static_assert(ESP32_BLE_DEVICE_ID >= DEVICE_ID_MIN,
               "ESP32_BLE_DEVICE_ID must be at least 1");
_Static_assert(ESP32_BLE_DEVICE_ID <= DEVICE_ID_MAX,
               "ESP32_BLE_DEVICE_ID must not exceed 32766");
_Static_assert(DEVICE_UUID_RESERVED_OFFSET <= ESP_BLE_MESH_OCTET16_LEN,
               "Device UUID layout exceeds 16 bytes");

static uint8_t s_device_uuid[ESP_BLE_MESH_OCTET16_LEN];

static esp_ble_mesh_cfg_srv_t s_config_server = {
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

/*
 * A relay forwards Network PDUs without parsing these Access messages. The
 * current Gateway still binds and configures model 0x0002 for every managed
 * node, so this passive model keeps the relay compatible with that automatic
 * configuration chain.
 */
static esp_ble_mesh_model_op_t s_vendor_ops[] = {
    ESP_BLE_MESH_MODEL_OP(IMAGE_OPCODE(BLE_MESH_IMAGE_OP_ACCEPT),
                          sizeof(ble_mesh_image_frame_t)),
    ESP_BLE_MESH_MODEL_OP(IMAGE_OPCODE(BLE_MESH_IMAGE_OP_BUSY),
                          sizeof(ble_mesh_image_frame_t)),
    ESP_BLE_MESH_MODEL_OP(IMAGE_OPCODE(BLE_MESH_IMAGE_OP_COMPLETE),
                          sizeof(ble_mesh_image_frame_t)),
    ESP_BLE_MESH_MODEL_OP(IMAGE_OPCODE(BLE_MESH_IMAGE_OP_NACK),
                          sizeof(ble_mesh_image_nack_header_t) + 1U),
    ESP_BLE_MESH_MODEL_OP(IMAGE_OPCODE(BLE_MESH_IMAGE_OP_RESTART),
                          sizeof(ble_mesh_image_frame_t)),
    ESP_BLE_MESH_MODEL_OP(IMAGE_OPCODE(BLE_MESH_IMAGE_OP_REJECT),
                          sizeof(ble_mesh_image_reject_t)),
    ESP_BLE_MESH_MODEL_OP(IMAGE_OPCODE(BLE_MESH_IMAGE_OP_TIME_STATUS),
                          sizeof(ble_mesh_time_status_message_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(s_relay_publication, 1, ROLE_NODE);

static esp_ble_mesh_model_t s_vendor_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(BLE_MESH_IMAGE_COMPANY_ID,
                              BLE_MESH_IMAGE_SOURCE_MODEL_ID,
                              s_vendor_ops, &s_relay_publication, NULL),
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
    .uuid = s_device_uuid,
};

static void build_device_uuid(const uint8_t bt_addr[DEVICE_UUID_BT_ADDR_BYTES])
{
    memset(s_device_uuid, 0, sizeof(s_device_uuid));
    s_device_uuid[0] = DEVICE_UUID_PREFIX_0;
    s_device_uuid[1] = DEVICE_UUID_PREFIX_1;
    memcpy(s_device_uuid + DEVICE_UUID_BT_ADDR_OFFSET, bt_addr,
           DEVICE_UUID_BT_ADDR_BYTES);
    s_device_uuid[DEVICE_UUID_ID_OFFSET] =
        (uint8_t)((uint16_t)ESP32_BLE_DEVICE_ID & 0xFFU);
    s_device_uuid[DEVICE_UUID_ID_OFFSET + 1U] =
        (uint8_t)(((uint16_t)ESP32_BLE_DEVICE_ID >> 8U) & 0xFFU);
}

static void log_device_identity(void)
{
    ESP_LOGI(TAG,
             "device_id=%u requested_unicast=0x%04" PRIx16
             " bt=%02x:%02x:%02x:%02x:%02x:%02x",
             (unsigned int)ESP32_BLE_DEVICE_ID, EXPECTED_UNICAST_ADDR,
             s_device_uuid[2], s_device_uuid[3], s_device_uuid[4],
             s_device_uuid[5], s_device_uuid[6], s_device_uuid[7]);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_device_uuid, sizeof(s_device_uuid),
                             ESP_LOG_INFO);
}

static esp_err_t enable_unprovisioned_bearers(void)
{
    return esp_ble_mesh_node_prov_enable(
        (esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV |
                                     ESP_BLE_MESH_PROV_GATT));
}

static void provisioning_callback(esp_ble_mesh_prov_cb_event_t event,
                                  esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "Mesh registration complete, status=%d",
                 param->prov_register_comp.err_code);
        break;
    case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "Provisioning advertising status=%d",
                 param->node_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "Provisioning link opened (%s)",
                 param->node_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV ?
                     "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT:
        ESP_LOGI(TAG, "Provisioning link closed (%s)",
                 param->node_prov_link_close.bearer == ESP_BLE_MESH_PROV_ADV ?
                     "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG,
                 "Provisioned: net_idx=0x%03x addr=0x%04x flags=0x%02x "
                 "iv_index=0x%08" PRIx32,
                 param->node_prov_complete.net_idx,
                 param->node_prov_complete.addr,
                 param->node_prov_complete.flags,
                 param->node_prov_complete.iv_index);
        ESP_LOGI(TAG, "Relay waits for Gateway configuration");
        break;
    case ESP_BLE_MESH_NODE_PROV_RESET_EVT: {
        ESP_LOGW(TAG, "Mesh node reset; advertising for provisioning again");
        esp_err_t err = enable_unprovisioned_bearers();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to re-enable provisioning: %s",
                     esp_err_to_name(err));
        }
        break;
    }
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

    ESP_LOGI(TAG, "Configuration changed, opcode=0x%08" PRIx32,
             param->ctx.recv_op);
    if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_RELAY_SET) {
        ESP_LOGI(TAG, "Relay feature configured by Gateway");
    } else if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET) {
        ESP_LOGI(TAG, "Image model configuration complete; relay ready");
    }
}

static void custom_model_callback(esp_ble_mesh_model_cb_event_t event,
                                  esp_ble_mesh_model_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT || param == NULL ||
        param->model_operation.ctx == NULL) {
        return;
    }

    ESP_LOGW(TAG,
             "Access message addressed to relay: opcode=0x%08" PRIx32
             " source=0x%04x length=%u (ignored)",
             param->model_operation.opcode,
             param->model_operation.ctx->addr,
             (unsigned int)param->model_operation.length);
}

static esp_err_t bluetooth_init(void)
{
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        return err;
    }

    esp_bt_controller_config_t controller_config =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&controller_config);
    if (err == ESP_OK) {
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    }
    if (err == ESP_OK) {
        err = esp_bluedroid_init();
    }
    if (err == ESP_OK) {
        err = esp_bluedroid_enable();
    }
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t *bt_addr = esp_bt_dev_get_address();
    if (bt_addr == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    build_device_uuid(bt_addr);
    return ESP_OK;
}

static esp_err_t mesh_init(void)
{
    esp_err_t err = esp_ble_mesh_register_prov_callback(
        provisioning_callback);
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
        return err;
    }

    if (esp_ble_mesh_node_is_provisioned()) {
        ESP_LOGI(TAG, "Stored Mesh state restored; relay is provisioned");
        return ESP_OK;
    }

    return enable_unprovisioned_bearers();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32 BLE Mesh relay");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bluetooth_init());
    log_device_identity();
    ESP_ERROR_CHECK(mesh_init());

    ESP_LOGI(TAG,
             "BLE Mesh relay initialized (%s)",
             esp_ble_mesh_node_is_provisioned() ?
                 "provisioned" : "waiting for provisioning");
}
