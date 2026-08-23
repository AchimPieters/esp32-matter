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

# Pull in the esp-matter components. Deliberately does NOT include
# $ESP_MATTER_PATH/examples/common like the official examples do: ESP-IDF's
# default component discovery tries to build every component it finds in
# EXTRA_COMPONENT_DIRS, and examples/common/app_reset requires a "button"
# component (espressif/button from the component registry) that isn't
# declared anywhere in this minimal project. Since app_main.cpp doesn't use
# anything from examples/common (this device reads its DS18B20 sensor via
# plain bit-banged 1-Wire on driver/gpio.h, not any example-only helper
# components), leaving it out avoids that unresolved dependency entirely.
set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/components"
    "${MATTER_SDK_PATH}/config/esp32/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The connectedhomeip/esp_matter sources are compiled as their own component,
# so this flag has to be set project-wide (not just on our own component in
# main/CMakeLists.txt) — every official esp-matter example sets it the same
# way, right here, before project(). Without it, platform headers fail to
# compile (missing platform-specific implementation types) because they're
# gated on CHIP_HAVE_CONFIG_H being defined.
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)

project(matter_water_freeze_detector)
