#pragma once

/*
 * Give every physical ESP32 relay a unique installation ID before building.
 * The Gateway assigns Mesh unicast address ESP32_BLE_DEVICE_ID + 1.
 *
 * The default 100 avoids the low IDs normally used by camera source nodes.
 * Valid range: 1..32766.
 */
#ifndef ESP32_BLE_DEVICE_ID
#define ESP32_BLE_DEVICE_ID 100U
#endif
