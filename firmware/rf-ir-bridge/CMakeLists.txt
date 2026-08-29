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

# Pull in the esp-matter components, PLUS one single extra component this
# device type genuinely needs and most other device types in this repo
# don't: `examples/common/app_bridge`, the generic dynamic-Matter-endpoint
# machinery every real esp-matter bridge example (including firmware/
# ble-mesh-bridge/'s own verbatim-ported one) builds on. Deliberately
# pointed at that ONE component's own directory, not the whole
# `$ESP_MATTER_PATH/examples/common` folder the way Espressif's own
# bridge examples do it — see firmware/light/'s own CMakeLists.txt comment
# for the real, previously-confirmed gotcha that blanket-including
# `examples/common` risks (its own `app_reset` subdirectory needing a
# `button` component this project doesn't declare). Pointing at
# `app_bridge` directly registers only that one component, sidestepping
# the question of what else lives under `examples/common` entirely.
set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/components"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/examples/common/app_bridge")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The connectedhomeip/esp_matter sources are compiled as their own component,
# so this flag has to be set project-wide (not just on our own component in
# main/CMakeLists.txt) — every official esp-matter example sets it the same
# way, right here, before project(). Without it, headers like
# connectedhomeip/src/include/platform/PlatformManager.h fail to compile
# (missing platform-specific implementation types) because they're gated on
# CHIP_HAVE_CONFIG_H being defined.
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)

project(matter_rf_ir_bridge)
