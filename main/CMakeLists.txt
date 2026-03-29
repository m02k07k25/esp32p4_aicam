idf_build_get_property(component_targets __COMPONENT_TARGETS)
if ("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
elseif("___idf_esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
else()
    message(FATAL_ERROR "esp-dl component not found. Check main/idf_component.yml.")
endif()

set(cmake_dir ${espdl_dir}/fbs_loader/cmake)
include(${cmake_dir}/utilities.cmake)

set(srcs
    "simple_video_server_example.c"
    "infer_bridge.cpp")

set(classifier_model "${CMAKE_CURRENT_LIST_DIR}/../model/artifacts/espdl/classifier_224_p4.espdl")
if (NOT EXISTS "${classifier_model}")
    message(FATAL_ERROR
        "Missing classifier model: ${classifier_model}\n"
        "Generate model/artifacts/espdl/classifier_224_p4.espdl with model/quantize_espdl.py before building.")
endif()

idf_component_register(SRCS ${srcs}
                       PRIV_INCLUDE_DIRS .
                       REQUIRES esp-dl
                       PRIV_REQUIRES nvs_flash
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

target_add_aligned_binary_data(${COMPONENT_LIB}
                               "${classifier_model}"
                               BINARY
                               RENAME_TO classifier_224_p4_espdl)
