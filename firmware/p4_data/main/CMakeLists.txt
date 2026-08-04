set(srcs
    "simple_video_server_example.c")

idf_component_register(SRCS ${srcs}
                       PRIV_INCLUDE_DIRS .
                       PRIV_REQUIRES nvs_flash
                                     esp_event
                                     esp_netif
                                     esp_http_server
                                     esp_wifi
                                     esp_video
                                     esp_driver_jpeg
                                     fatfs
                                     sdmmc
                                     esp_driver_sdmmc
                                     mdns
                                     lwip
                                     esp_timer
                                     freertos
                                     heap
                                     log
                                     esp_common
                                     esp_hosted)
