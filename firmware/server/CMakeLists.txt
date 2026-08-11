cmake_minimum_required(VERSION 3.16)

# Deliberately do not force IDF_TARGET. Select a supported chip with:
#   idf.py set-target esp32|esp32c3|esp32c6|esp32c61|esp32h2|esp32s3
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/ble_mesh_image_protocol")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mesh_image_server)
