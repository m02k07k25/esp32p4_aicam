set(hosted_common_dir "${CMAKE_CURRENT_SOURCE_DIR}/common")

set(hosted_sources
    "${hosted_common_dir}/proto/esp_hosted_rpc.pb-c.c"
    "esp_hosted_coprocessor.c"
    "protocomm_pserial.c"
    "mempool.c"
    "mempool_ll.c"
    "stats.c"
    "slave_transport_gpio_pin_guard.c"
    "slave_control.c"
    "sdio_slave_api.c")

idf_component_register(
    SRCS
        ${hosted_sources}
        "sdio_frame_receiver.c"
        "ble_mesh_image_source.c"
        "app_main.c"
    INCLUDE_DIRS
        "."
        "${hosted_common_dir}"
        "${hosted_common_dir}/log"
        "${hosted_common_dir}/proto"
        "${hosted_common_dir}/rpc"
        "${hosted_common_dir}/transport"
        "${hosted_common_dir}/utils"
    REQUIRES
        app_update
        bt
        esp_driver_gpio
        esp_driver_sdio
        esp_event
        esp_partition
        esp_hw_support
        esp_rom
        esp_timer
        nvs_flash
        protocomm
        protobuf-c
        ble_mesh_image_protocol
        sdio_frame_protocol)

# ESP-Hosted's private serial protocomm transport uses protocomm_priv.h.
idf_component_get_property(protocomm_dir protocomm COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${protocomm_dir}/src/common")
