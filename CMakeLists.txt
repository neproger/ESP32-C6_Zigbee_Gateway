# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32-c6_ZBGW)

# Web UI (SPIFFS image)
#
# If `web/dist` exists, it will be packed into the `www` SPIFFS partition at build time.
# Flash together with firmware using `idf.py flash` (image is included via FLASH_IN_PROJECT).
if(EXISTS "${CMAKE_SOURCE_DIR}/web/dist/index.html")
    spiffs_create_partition_image(www "${CMAKE_SOURCE_DIR}/web/dist" FLASH_IN_PROJECT)
endif()
