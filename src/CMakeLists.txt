idf_component_register(
    SRCS "main.cpp" "logbuf.cpp" "recovery.cpp" "diagnostics.cpp" "panel_config.cpp" "netwatch.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_netif lwip esp_adc esp_app_format app_update spi_flash espcoredump esp_partition esp_timer
             app_config app_net app_web app_store app_openhab panel app_display nextion board
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
