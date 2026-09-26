# The test app reaches into the library's internal headers on purpose: it
# exercises the links directly, before the BtpDevice core. Those headers pull
# in components the library only needs privately (esp_event through
# WifiStation.h), so they are listed here too -- idf.py enforces it, PlatformIO
# does not.
idf_component_register(SRCS "main.cpp"
                       REQUIRES BTPDevice BTP
                       PRIV_REQUIRES esp_timer esp_event)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../src")
