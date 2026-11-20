idf_component_register(SRCS "main.cpp" "relay.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES TraceKit
                       PRIV_REQUIRES esp_timer esp_driver_gpio)
