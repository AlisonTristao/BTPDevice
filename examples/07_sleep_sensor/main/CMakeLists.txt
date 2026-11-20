idf_component_register(SRCS "main.cpp" "chip_temp.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES BTPDevice
                       PRIV_REQUIRES esp_driver_tsens)
