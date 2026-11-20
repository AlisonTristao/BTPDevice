idf_component_register(SRCS "main.cpp" "btp/btp_commands.cpp" "btp/btp_topics.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES TraceKit
                       PRIV_REQUIRES esp_timer)
