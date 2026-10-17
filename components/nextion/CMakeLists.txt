idf_component_register(
    SRCS "src/Protocol.cpp" "src/Display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_gpio esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
