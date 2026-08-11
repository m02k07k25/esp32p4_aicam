#!/usr/bin/env python3
"""Small source-level guards for gateway paths that require a real Mesh stack."""

from pathlib import Path
import re


HERE = Path(__file__).resolve().parent
SOURCE = (HERE.parent / "main" / "mesh_image_gateway.c").read_text(
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
    r"enable_provisioning\s*\([^)]*\).*?resume_node_configuration\s*\(\s*\)",
    "restored nodes do not resume their idempotent config chain",
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
    r"custom_model_callback\s*\([^)]*\).*?"
    r"BLE_MESH_IMAGE_OP_OPEN.*?sample_time_provider\s*\(.*?"
    r"image_reassembly_receive\s*\(.*?"
    r"replies\[0\]\.opcode == BLE_MESH_IMAGE_OP_ACCEPT.*?"
    r"image_reassembly_set_rx_estimate\s*\(",
    "timestamp-less OPEN is not assigned its first receive-time estimate",
)

print("PASS: persisted-node restore and safe AppKey handling")
print("PASS: publication remains the final configuration step")
print("PASS: HTTP/SNTP idle-work reservation blocks racing OPEN")
print("PASS: unsolicited client-model publication event is handled")
print("PASS: RX estimate is sampled at OPEN, before JPEG completion")
