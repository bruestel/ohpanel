idf_component_register(
    SRCS "src/OpenHab.cpp" "src/Client.cpp" "src/States.cpp" "src/Items.cpp" "src/Options.cpp" "src/Discover.cpp" "src/Scan.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app_config esp_http_client esp_timer mbedtls esp-tls mdns
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
