cmake_minimum_required(VERSION 3.16)

# This version string is embedded in the firmware and used by Matter OTA to
# decide whether an update is newer. Bump it on every release.
set(PROJECT_VER "1.0.0")

# ESP_MATTER_PATH and IDF_PATH are already set inside the espressif/esp-matter
# Docker image. If you build outside Docker, export them yourself first.
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR
        "ESP_MATTER_PATH is not set. Build inside the espressif/esp-matter Docker "
        "image (see README), or export ESP_MATTER_PATH manually.")
endif()

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# Pull in the esp-matter components. This mirrors the layout used by the
# official esp-matter examples. If a future SDK version moves things around,
# copy the EXTRA_COMPONENT_DIRS block from any example under
# $ESP_MATTER_PATH/examples/.
set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/components"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/examples/common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(matter_light)
