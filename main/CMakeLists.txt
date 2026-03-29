set(srcs
    "simple_video_server_example.c"
    "yolo_bridge.cpp")

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS .
                       REQUIRES coco_detect
                       PRIV_REQUIRES esp-dl
                                     nvs_flash
                                     esp_event
                                     esp_http_server
                                     protocol_examples_common
                                     esp_video
                                     esp_driver_jpeg
                                     mdns
                                     lwip
                                     esp_timer
                                     freertos
                                     heap
                                     log
                                     esp_common)
