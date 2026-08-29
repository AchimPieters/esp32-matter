idf_component_register(SRCS "app_main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES app_bridge)

# -Wno-format-nonliteral / -DCHIP_HAVE_CONFIG_H etc. are set project-wide in
# the root CMakeLists.txt, since connectedhomeip/esp_matter compile as their
# own component and need them too — not just this one.
