#!/usr/bin/env python3
"""Small source-level guards for gateway paths that require a real Mesh stack."""

from pathlib import Path
import re


HERE = Path(__file__).resolve().parent
SOURCE = (HERE.parent / "main" / "mesh_image_gateway.c").read_text(
    encoding="utf-8"
)
WIFI_SOURCE = (HERE.parent / "main" / "server_wifi_time_adapter.c").read_text(
    encoding="utf-8"
)
SERIAL_SOURCE = (HERE.parent / "main" / "server_serial_adapter.c").read_text(
    encoding="utf-8"
)
HTTP_SOURCE = (HERE.parent / "main" / "server_http_adapter.c").read_text(
    encoding="utf-8"
)
APP_SOURCE = (HERE.parent / "main" / "app_main.c").read_text(
    encoding="utf-8"
)
KCONFIG = (HERE.parent / "main" / "Kconfig.projbuild").read_text(
    encoding="utf-8"
)


def require(pattern: str, description: str) -> None:
    if re.search(pattern, SOURCE, re.DOTALL) is None:
        raise AssertionError(description)


def between(start: str, end: str) -> str:
    begin = SOURCE.index(start)
    finish = SOURCE.index(end, begin)
    return SOURCE[begin:finish]


# A provisioner reboot must recover nodes from BLE Mesh settings and resume
# configuration even though provisioned nodes no longer advertise.
require(
    r"restore_provisioned_nodes\s*\([^)]*\).*?"
    r"esp_ble_mesh_provisioner_get_node_table_entry\s*\(",
    "persisted provisioner nodes are not enumerated",
)
require(
    r"mesh_image_gateway_init\s*\([^)]*\).*?restore_provisioned_nodes\s*\(\s*\)",
    "gateway init does not restore the persisted node table",
)
require(
    r"mark_gateway_ready\s*\([^)]*\).*?resume_node_configuration\s*\(\s*\)",
    "restored nodes do not resume their idempotent config chain",
)

# In ESP-IDF 5.5 PROV_ENABLE creates/restores Primary NetKey index 0. AppKey
# lookup/add before its completion fails asynchronously with -ENODEV. Start the
# Provisioner first, finish local key setup from its completion event, and do
# not admit an unprovisioned C6 during the short asynchronous bind window.
gateway_init = SOURCE[SOURCE.index("esp_err_t mesh_image_gateway_init"):]
if "esp_ble_mesh_provisioner_prov_enable" not in gateway_init:
    raise AssertionError("gateway init does not create/restore its primary network")
for premature in (
    "esp_ble_mesh_provisioner_get_local_app_key",
    "esp_ble_mesh_provisioner_add_local_app_key",
    "bind_local_gateway_model()",
):
    if premature in gateway_init:
        raise AssertionError(f"gateway init performs {premature} before PROV_ENABLE completes")
provisioning = between("static void provisioning_callback", "static void config_client_callback")
enable_case = between(
    "case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT",
    "case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT",
)
for required in (
    "provisioner_prov_enable_comp.err_code",
    "local_gateway_model_is_bound()",
    "esp_ble_mesh_provisioner_get_local_app_key",
    "load_or_create_app_key()",
    "esp_ble_mesh_provisioner_add_local_app_key",
):
    if required not in enable_case:
        raise AssertionError(f"PROV_ENABLE completion is missing: {required}")
require(
    r"PROVISIONER_RECV_UNPROV_ADV_PKT_EVT.*?!s_gateway_ready.*?break",
    "unprovisioned devices are accepted before the local AppKey/model is ready",
)

# The compile-time C6 ID embedded in UUID[8..9] owns a deterministic address:
# ID 1 -> 0x0002.  Never mix the allocator-based API into this path.
if "esp_ble_mesh_provisioner_prov_device_with_addr" not in provisioning:
    raise AssertionError("C6 provisioning does not request its deterministic address")
if "esp_ble_mesh_provisioner_add_unprov_dev" in provisioning:
    raise AssertionError("fixed and allocator-based provisioning APIs are mixed")
require(
    r"device_id_from_uuid\s*\([^)]*\).*?DEVICE_UUID_ID_OFFSET.*?"
    r"device_addr_from_id\s*\([^)]*\).*?SERVER_UNICAST_ADDR \+ device_id",
    "UUID device ID is not mapped to address device_id + 1",
)
require(
    r"PROVISIONER_PROV_COMPLETE_EVT.*?store_node\s*\(",
    "provisioning completion does not validate/store the fixed identity",
)
require(
    r"PROVISIONER_PROV_LINK_CLOSE_EVT.*?"
    r"s_provisioning_active\s*=\s*false.*?s_pending_device_id\s*=\s*0U",
    "the fixed-address reservation is not released at link close",
)
complete_case = between(
    "case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT",
    "case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT",
)
if "s_provisioning_active = false" in complete_case:
    raise AssertionError(
        "provisioning reservation is released before the old link closes"
    )
if "s_pending_device_uuid" not in complete_case:
    raise AssertionError("provisioning completion is not matched to its UUID")
require(
    r"RECV_UNPROV_ADV_PKT_EVT.*?has_free_runtime_node_slot\s*\(\s*\).*?"
    r"provisioner_prov_device_with_addr",
    "runtime node capacity is not checked before provisioning",
)
require(
    r"find_mesh_node_by_device_id\s*\([^)]*\).*?"
    r"provisioner_prov_device_with_addr",
    "persisted duplicate device IDs are not rejected",
)

# Re-adding an identical persisted AppKey returns SUCCESS. Status 0x06 can
# mean a different key already occupies the index and must never advance to
# model bind/READY.
callback = between("static void config_client_callback", "static uint8_t raw_image_opcode")
appkey_begin = callback.index(
    "if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {",
)
appkey_end = callback.index(
    "} else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {",
    appkey_begin,
)
appkey_branch = callback[appkey_begin:appkey_end]
if "status != ESP_BLE_MESH_CFG_STATUS_SUCCESS" not in appkey_branch:
    raise AssertionError("AppKey status is not checked for SUCCESS")
status_condition = appkey_branch[
    appkey_branch.index("if (status"):
    appkey_branch.index(") {", appkey_branch.index("if (status"))
]
if "KEY_INDEX_ALREADY_STORED" in status_condition:
    raise AssertionError("conflicting AppKey index is accepted as success")

# Publication must be the final executed step, because it makes C6 READY.
transitions = [
    ("APP_KEY_ADD", "MODEL_APP_BIND"),
    ("MODEL_APP_BIND", "RELAY_SET"),
    ("RELAY_SET", "NETWORK_TRANSMIT_SET"),
    ("NETWORK_TRANSMIT_SET", "DEFAULT_TTL_SET"),
    ("DEFAULT_TTL_SET", "MODEL_PUB_SET"),
]
for current, following in transitions:
    require(
        rf"if \(opcode == ESP_BLE_MESH_MODEL_OP_{current}\).*?"
        rf"send_node_config\s*\(.*?ESP_BLE_MESH_MODEL_OP_{following}\)",
        f"configuration does not transition {current} -> {following}",
    )
require(
    r"if \(opcode == ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET\).*?"
    r"C6 image model ready",
    "publication success does not mark the image model ready",
)

# Local bulk work and OPEN are serialized by the same gateway reservation.
require(
    r"mesh_image_gateway_try_begin_idle_work\s*\([^)]*\).*?"
    r"!s_reassembly\.active && !s_idle_work_reserved",
    "idle-work reservation is not acquired atomically with reassembly state",
)
require(
    r"custom_model_callback\s*\([^)]*\).*?s_idle_work_reserved.*?"
    r"BLE_MESH_IMAGE_OP_BUSY",
    "OPEN is not held off while local bulk work owns the radio window",
)

# A vendor model initialized with esp_ble_mesh_client_model_init receives
# unsolicited C6 publication traffic through the client publish event union.
require(
    r"custom_model_callback\s*\([^)]*\).*?"
    r"ESP_BLE_MESH_CLIENT_MODEL_RECV_PUBLISH_MSG_EVT.*?"
    r"param->client_recv_publish_msg\.opcode.*?"
    r"param->client_recv_publish_msg\.msg",
    "unsolicited vendor publication callback path is not decoded",
)
require(
    r"custom_model_callback\s*\([^)]*\).*?find_node\s*\(context->addr\).*?"
    r"source_node\s*==\s*NULL.*?device_addr_from_id",
    "image traffic is not gated by the validated device ID/address table",
)

# Managed C6 nodes can request server time without opening an image session.
# The response is always explicit: synchronized clocks return both receive and
# transmit Unix times, while unavailable clocks return two zero timestamps.
require(
    r"s_vendor_ops.*?BLE_MESH_IMAGE_OP_TIME_REQUEST.*?"
    r"sizeof\(ble_mesh_time_request_t\)",
    "TIME_REQUEST is not registered on the vendor model",
)
require(
    r"raw_image_opcode\s*\([^)]*\).*?BLE_MESH_IMAGE_OP_TIME_REQUEST",
    "TIME_REQUEST opcode is not decoded",
)
custom_callback = between("static void custom_model_callback", "static void timeout_task")
if custom_callback.index("source_node == NULL") > custom_callback.index(
    "handle_time_request"
):
    raise AssertionError("TIME_REQUEST bypasses managed C6 identity validation")
time_handler = between("static void handle_time_request", "static void publish_complete")
for required in (
    "message_len != sizeof(ble_mesh_time_request_t)",
    "ble_mesh_image_get_le32(message)",
    "BLE_MESH_TIME_STATUS_UNAVAILABLE",
    "BLE_MESH_TIME_STATUS_OK",
    "sample_time_provider(&server_rx_unix_ms)",
    "sample_time_provider(&server_tx_unix_ms)",
    "server_tx_unix_ms >= server_rx_unix_ms",
    "ble_mesh_image_put_le64(status + 8U, server_rx_unix_ms)",
    "ble_mesh_image_put_le64(status + 16U, server_tx_unix_ms)",
    "BLE_MESH_IMAGE_OP_TIME_STATUS",
):
    if required not in time_handler:
        raise AssertionError(f"TIME_STATUS path is missing: {required}")
if "uint8_t status[sizeof(ble_mesh_time_status_message_t)] = {0}" not in time_handler:
    raise AssertionError("TIME_STATUS reserved bytes/unavailable timestamps are not zeroed")

# Wi-Fi/SNTP is an independent, persistent clock adapter. Serial output may be
# enabled at the same time, while HTTP reuses this owner instead of initializing
# a second Wi-Fi or SNTP instance.
if "depends on SOC_WIFI_SUPPORTED && !SERVER_SERIAL_IMAGE_ENABLE" in KCONFIG[
    KCONFIG.index("config SERVER_WIFI_SNTP_ENABLE"):
    KCONFIG.index("config SERVER_HTTP_ENABLE")
]:
    raise AssertionError("Wi-Fi/SNTP is still mutually exclusive with serial output")
for required in (
    "server_serial_adapter_init()",
    "server_wifi_time_adapter_init()",
):
    if required not in APP_SOURCE:
        raise AssertionError(f"app_main does not initialize {required}")
for required in (
    "mesh_image_gateway_set_time_provider(wall_clock_now, NULL)",
    "esp_netif_sntp_init(&sntp)",
    "sntp.start = false",
    "sntp.sync_cb = time_synchronized",
    "esp_netif_sntp_start()",
    "CONFIG_SERVER_SNTP_MAX_AGE_MS",
    "s_last_sync_monotonic_us",
    "WIFI_EVENT_STA_DISCONNECTED",
    "schedule_reconnect()",
    "esp_wifi_set_storage(WIFI_STORAGE_RAM)",
    "esp_wifi_set_ps(WIFI_PS_MIN_MODEM)",
):
    if required not in WIFI_SOURCE:
        raise AssertionError(f"persistent Wi-Fi/SNTP adapter is missing: {required}")
if "esp_netif_sntp_deinit" in WIFI_SOURCE:
    raise AssertionError("persistent SNTP is deinitialized after its first update")
for forbidden in (
    "esp_wifi_init",
    "esp_netif_sntp_init",
    "mesh_image_gateway_set_time_provider",
):
    if forbidden in HTTP_SOURCE:
        raise AssertionError(f"HTTP duplicates network/time ownership: {forbidden}")
require(
    r"custom_model_callback\s*\([^)]*\).*?"
    r"BLE_MESH_IMAGE_OP_OPEN.*?sample_time_provider\s*\(.*?"
    r"image_reassembly_receive\s*\(.*?"
    r"replies\[0\]\.opcode == BLE_MESH_IMAGE_OP_ACCEPT.*?"
    r"image_reassembly_set_rx_estimate\s*\(",
    "timestamp-less OPEN is not assigned its first receive-time estimate",
)

print("PASS: persisted-node restore and safe AppKey handling")
print("PASS: compile-time C6 IDs receive deterministic Mesh addresses")
print("PASS: publication remains the final configuration step")
print("PASS: HTTP idle-work reservation blocks racing OPEN")
print("PASS: unsolicited client-model publication event is handled")
print("PASS: RX estimate is sampled at OPEN, before JPEG completion")
print("PASS: managed C6 TIME_REQUEST returns explicit two-timestamp status")
print("PASS: serial export and persistent Wi-Fi/SNTP have independent ownership")

if re.search(
    r"heap_caps_calloc\s*\(\s*SERIAL_SLOT_COUNT\s*,\s*sizeof\(\*s_slots\)\s*,\s*slot_caps\s*\)",
    SERIAL_SOURCE,
) is None:
    raise AssertionError("serial JPEG slot is not preallocated once from the heap")
if "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" not in SERIAL_SOURCE:
    raise AssertionError("serial JPEG slot must use byte-addressable internal RAM")
if re.search(r"static\s+serial_image_slot_t\s+s_slots\s*\[", SERIAL_SOURCE):
    raise AssertionError("serial JPEG slot still consumes .dram0.bss")
print("PASS: serial JPEG callback uses a bounded init-time internal-RAM slot")
