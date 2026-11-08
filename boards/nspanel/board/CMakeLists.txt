# The Sonoff NSPanel's hardware: its pins (relays, buttons, the display's power, the NTC, the buzzer), the
# temperature sensor and the buzzer. The board is chosen in the project's CMakeLists.txt (OHPANEL_BOARD).
idf_component_register(
    SRCS "temperature.cpp" "buzzer.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_ledc esp_adc app_config app_openhab
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
