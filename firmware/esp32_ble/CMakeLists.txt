cmake_minimum_required(VERSION 3.16)

# This project is intentionally limited to the original ESP32 target. Keeping
# the target here prevents an ESP32-C6/S3 binary from being built by accident.
set(IDF_TARGET esp32 CACHE STRING "ESP-IDF target" FORCE)
set(PROJECT_VER "1.0.0")

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/ble_mesh_image_protocol")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_ble)
