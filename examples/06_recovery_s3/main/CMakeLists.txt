idf_component_register(SRCS "main.cpp" "rgb_led.cpp" "chip_temp.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES TraceKit
                       PRIV_REQUIRES esp_driver_tsens nvs_flash)
