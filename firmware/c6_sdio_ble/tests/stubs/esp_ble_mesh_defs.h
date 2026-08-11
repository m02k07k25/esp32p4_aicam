#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#define ESP_BLE_MESH_OCTET16_LEN 16U
#define ESP_BLE_MESH_KEY_UNUSED UINT16_C(0xffff)

#define ESP_BLE_MESH_RELAY_ENABLED              1U
#define ESP_BLE_MESH_BEACON_ENABLED             1U
#define ESP_BLE_MESH_GATT_PROXY_ENABLED         1U
#define ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED   0U
#define ESP_BLE_MESH_FRIEND_ENABLED             1U
#define ESP_BLE_MESH_FRIEND_NOT_SUPPORTED       0U
#define ESP_BLE_MESH_PROV_ADV                    0x01U
#define ESP_BLE_MESH_PROV_GATT                   0x02U
#define ROLE_NODE                                 0U

#define ESP_BLE_MESH_TRANSMIT(count, interval) \
    ((uint8_t)(((count) & 0x07U) | (((interval) / 10U) << 3U)))
#define ESP_BLE_MESH_MODEL_OP_3(opcode, company_id) \
    ((uint32_t)(opcode) | ((uint32_t)(company_id) << 8U))

typedef struct {
    uint8_t net_transmit;
    uint8_t relay;
    uint8_t relay_retransmit;
    uint8_t beacon;
    uint8_t gatt_proxy;
    uint8_t friend_state;
    uint8_t default_ttl;
} esp_ble_mesh_cfg_srv_t;

typedef struct {
    uint32_t opcode;
    size_t min_len;
} esp_ble_mesh_model_op_t;

typedef struct {
    uint16_t publish_addr;
    uint16_t app_idx;
    uint8_t ttl;
} esp_ble_mesh_model_pub_t;

typedef struct {
    uint16_t keys[4];
    uint16_t company_id;
    uint16_t model_id;
    const esp_ble_mesh_model_op_t *ops;
    esp_ble_mesh_cfg_srv_t *config_server;
    esp_ble_mesh_model_pub_t *pub;
} esp_ble_mesh_model_t;

typedef struct {
    uint16_t element_addr;
    esp_ble_mesh_model_t *root_models;
    esp_ble_mesh_model_t *vendor_models;
} esp_ble_mesh_elem_t;

typedef struct {
    uint16_t cid;
    size_t element_count;
    esp_ble_mesh_elem_t *elements;
} esp_ble_mesh_comp_t;

typedef struct {
    uint8_t *uuid;
} esp_ble_mesh_prov_t;

#define ESP_BLE_MESH_MODEL_CFG_SRV(CONFIG_SERVER) \
    { .keys = { ESP_BLE_MESH_KEY_UNUSED, ESP_BLE_MESH_KEY_UNUSED, \
                ESP_BLE_MESH_KEY_UNUSED, ESP_BLE_MESH_KEY_UNUSED }, \
      .config_server = (CONFIG_SERVER) }
#define ESP_BLE_MESH_MODEL_OP(opcode_value, minimum_length) \
    { .opcode = (opcode_value), .min_len = (minimum_length) }
#define ESP_BLE_MESH_MODEL_OP_END \
    { .opcode = 0U, .min_len = 0U }
#define ESP_BLE_MESH_MODEL_PUB_DEFINE(NAME, MESSAGE_LENGTH, ROLE) \
    static esp_ble_mesh_model_pub_t NAME = {0}
#define ESP_BLE_MESH_VENDOR_MODEL(COMPANY_ID, MODEL_ID, OPS, PUB, USER_DATA) \
    { .keys = { ESP_BLE_MESH_KEY_UNUSED, ESP_BLE_MESH_KEY_UNUSED, \
                ESP_BLE_MESH_KEY_UNUSED, ESP_BLE_MESH_KEY_UNUSED }, \
      .company_id = (COMPANY_ID), .model_id = (MODEL_ID), .ops = (OPS), \
      .pub = (PUB) }
#define ESP_BLE_MESH_ELEMENT(LOCATION, ROOT_MODELS, VENDOR_MODELS) \
    { .element_addr = 0U, .root_models = (ROOT_MODELS), \
      .vendor_models = (VENDOR_MODELS) }

typedef uint8_t esp_ble_mesh_prov_bearer_t;

typedef struct {
    uint16_t net_idx;
    uint16_t app_idx;
    uint16_t addr;
    uint8_t send_ttl;
} esp_ble_mesh_msg_ctx_t;

typedef enum {
    ESP_BLE_MESH_PROV_REGISTER_COMP_EVT = 1,
    ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT,
    ESP_BLE_MESH_NODE_PROV_RESET_EVT,
} esp_ble_mesh_prov_cb_event_t;

typedef struct {
    struct {
        int err_code;
    } prov_register_comp;
    struct {
        uint16_t net_idx;
        uint16_t addr;
    } node_prov_complete;
} esp_ble_mesh_prov_cb_param_t;

typedef enum {
    ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT = 1,
} esp_ble_mesh_cfg_server_cb_event_t;

#define ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD       0x00U
#define ESP_BLE_MESH_MODEL_OP_APP_KEY_UPDATE    0x01U
#define ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND    0x02U
#define ESP_BLE_MESH_MODEL_OP_MODEL_APP_UNBIND  0x03U
#define ESP_BLE_MESH_MODEL_OP_APP_KEY_DELETE    0x04U
#define ESP_BLE_MESH_MODEL_OP_NET_KEY_DELETE    0x05U
#define ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET     0x06U
#define ESP_BLE_MESH_MODEL_OP_MODEL_PUB_VIRTUAL_ADDR_SET 0x07U

typedef struct {
    uint16_t net_idx;
    uint16_t app_idx;
} esp_ble_mesh_state_change_cfg_appkey_add_t;

typedef struct {
    uint16_t element_addr;
    uint16_t company_id;
    uint16_t model_id;
    uint16_t app_idx;
} esp_ble_mesh_state_change_cfg_model_app_bind_t;

typedef esp_ble_mesh_state_change_cfg_model_app_bind_t
    esp_ble_mesh_state_change_cfg_model_app_unbind_t;

typedef struct {
    uint16_t app_idx;
} esp_ble_mesh_state_change_cfg_appkey_delete_t;

typedef struct {
    uint16_t element_addr;
    uint16_t pub_addr;
    uint16_t app_idx;
    bool cred_flag;
    uint8_t pub_ttl;
    uint8_t pub_period;
    uint8_t pub_retransmit;
    uint16_t company_id;
    uint16_t model_id;
} esp_ble_mesh_state_change_cfg_mod_pub_set_t;

typedef struct {
    uint16_t element_addr;
    uint8_t label_uuid[16];
    uint16_t app_idx;
    bool cred_flag;
    uint8_t pub_ttl;
    uint8_t pub_period;
    uint8_t pub_retransmit;
    uint16_t company_id;
    uint16_t model_id;
} esp_ble_mesh_state_change_cfg_mod_pub_va_set_t;

typedef struct {
    struct {
        uint32_t recv_op;
    } ctx;
    struct {
        struct {
            esp_ble_mesh_state_change_cfg_appkey_add_t appkey_add;
            esp_ble_mesh_state_change_cfg_model_app_bind_t mod_app_bind;
            esp_ble_mesh_state_change_cfg_model_app_unbind_t mod_app_unbind;
            esp_ble_mesh_state_change_cfg_appkey_delete_t appkey_delete;
            esp_ble_mesh_state_change_cfg_mod_pub_set_t mod_pub_set;
            esp_ble_mesh_state_change_cfg_mod_pub_va_set_t mod_pub_va_set;
            struct {
                uint16_t net_idx;
            } netkey_delete;
        } state_change;
    } value;
} esp_ble_mesh_cfg_server_cb_param_t;

typedef enum {
    ESP_BLE_MESH_MODEL_SEND_COMP_EVT = 1,
    ESP_BLE_MESH_MODEL_OPERATION_EVT,
} esp_ble_mesh_model_cb_event_t;

typedef struct {
    struct {
        esp_ble_mesh_model_t *model;
        uint32_t opcode;
        esp_err_t err_code;
    } model_send_comp;
    struct {
        uint32_t opcode;
        esp_ble_mesh_model_t *model;
        size_t length;
        uint8_t *msg;
        esp_ble_mesh_msg_ctx_t *ctx;
    } model_operation;
} esp_ble_mesh_model_cb_param_t;

esp_err_t esp_ble_mesh_register_prov_callback(
    void (*callback)(esp_ble_mesh_prov_cb_event_t,
                     esp_ble_mesh_prov_cb_param_t *));
esp_err_t esp_ble_mesh_register_config_server_callback(
    void (*callback)(esp_ble_mesh_cfg_server_cb_event_t,
                     esp_ble_mesh_cfg_server_cb_param_t *));
esp_err_t esp_ble_mesh_register_custom_model_callback(
    void (*callback)(esp_ble_mesh_model_cb_event_t,
                     esp_ble_mesh_model_cb_param_t *));
esp_err_t esp_ble_mesh_init(esp_ble_mesh_prov_t *provision,
                            esp_ble_mesh_comp_t *composition);
esp_err_t esp_ble_mesh_node_prov_enable(esp_ble_mesh_prov_bearer_t bearers);
bool esp_ble_mesh_node_is_provisioned(void);
const uint8_t *esp_ble_mesh_node_get_local_net_key(uint16_t net_idx);
const uint8_t *esp_ble_mesh_node_get_local_app_key(uint16_t app_idx);
esp_err_t esp_ble_mesh_server_model_send_msg(esp_ble_mesh_model_t *model,
                                              esp_ble_mesh_msg_ctx_t *ctx,
                                              uint32_t opcode,
                                              uint16_t length,
                                              uint8_t *data);
