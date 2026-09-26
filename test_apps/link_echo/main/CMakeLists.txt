idf_component_register(SRCS "main.cpp" REQUIRES BTPDevice BTP PRIV_REQUIRES esp_timer)
# The test app reaches into the library's internal headers on purpose: it
# exercises the links directly, before the BtpDevice core exists.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../src")
