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

idf_component_register(
    SRCS
        ${server_sources}
    INCLUDE_DIRS "."
    REQUIRES
        ${server_requires})

# Component dependency expansion happens before project Kconfig values are
# available during a clean configure. Add these after registration so the
# conditional HTTP source gets the public headers without imposing a Wi-Fi
# dependency on non-Wi-Fi targets when the adapter is disabled.
if(CONFIG_SERVER_HTTP_ENABLE)
    idf_component_optional_requires(PRIVATE
        esp_event
        esp_http_server
        esp_netif
        esp_wifi
        lwip)
endif()
