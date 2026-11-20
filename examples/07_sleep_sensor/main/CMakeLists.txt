idf_component_register(SRCS "main.cpp" "chip_temp.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES TraceKit
                       PRIV_REQUIRES esp_driver_tsens)
