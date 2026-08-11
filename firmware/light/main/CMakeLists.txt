idf_component_register(SRCS "app_main.cpp"
                       INCLUDE_DIRS ".")

# esp-matter is C++ and uses exceptions/RTTI in a few places; keep warnings sane.
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format-nonliteral")
