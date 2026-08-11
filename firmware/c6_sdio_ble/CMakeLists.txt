cmake_minimum_required(VERSION 3.16)

# This firmware is intentionally C6-only: ESP32-C6 is the SDIO slave and runs
# the local NimBLE/ESP-BLE-Mesh stack.  Keeping the target here also prevents an
# accidental build for a chip whose SDIO slave pin mapping is different.
set(IDF_TARGET esp32c6 CACHE STRING "ESP-IDF target" FORCE)
set(PROJECT_VER "2.12.3")

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/sdio_frame_protocol"
    "${CMAKE_CURRENT_LIST_DIR}/../components/ble_mesh_image_protocol")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c6_sdio_ble)
