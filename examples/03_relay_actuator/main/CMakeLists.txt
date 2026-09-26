idf_component_register(SRCS "main.cpp" "btp/btp_commands.cpp" "btp/btp_topics.cpp" "relay.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES BTPDevice
                       PRIV_REQUIRES esp_timer esp_driver_gpio)
