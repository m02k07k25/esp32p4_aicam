# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# esp_wifi_remote 1.3.2 selects its IDF-specific Kconfig with this environment
# variable and expects major.minor rather than the 5.5.0 patch form.
set(ENV{ESP_IDF_VERSION} "5.5")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_data_capture)
