# The Sonoff NSPanel's hardware: its pins (relays, buttons, the display's power, the NTC), the
# temperature sensor. The board is chosen in the project's CMakeLists.txt (OHPANEL_BOARD).
idf_component_register(
    SRCS "temperature.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_adc app_config app_openhab
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
