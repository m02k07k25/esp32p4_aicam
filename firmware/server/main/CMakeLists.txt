set(server_sources
    "app_main.c"
    "image_reassembly.c"
    "mesh_image_gateway.c")
set(server_requires
    ble_mesh_image_protocol
    bt
    esp_hw_support
    esp_timer
    nvs_flash)

if(CONFIG_SERVER_HTTP_ENABLE)
    list(APPEND server_sources "server_http_adapter.c")
endif()

if(CONFIG_SERVER_SERIAL_IMAGE_ENABLE)
    list(APPEND server_sources "server_serial_adapter.c")
endif()

if(CONFIG_SERVER_SERIAL_TIME_ENABLE)
    list(APPEND server_sources "server_serial_time_adapter.c")
endif()

if(CONFIG_SERVER_WIFI_SNTP_ENABLE)
    list(APPEND server_sources "server_wifi_time_adapter.c")
endif()

idf_component_register(
    SRCS
        ${server_sources}
    INCLUDE_DIRS "."
    REQUIRES
        ${server_requires})

# Component dependency expansion happens before project Kconfig values are
# available during a clean configure. Add these after registration so the
# conditional adapters get their public headers without imposing Wi-Fi or HTTP
# dependencies on targets where the corresponding adapter is disabled.
if(CONFIG_SERVER_WIFI_SNTP_ENABLE)
    idf_component_optional_requires(PRIVATE
        esp_event
        esp_netif
        esp_wifi
        lwip)
endif()

if(CONFIG_SERVER_SERIAL_TIME_ENABLE)
    idf_component_optional_requires(PRIVATE esp_driver_uart)
endif()

if(CONFIG_SERVER_HTTP_ENABLE)
    idf_component_optional_requires(PRIVATE esp_http_server)
endif()
