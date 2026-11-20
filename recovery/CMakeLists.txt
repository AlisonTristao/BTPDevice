# TraceKit recovery firmware (docs/PLANO.md 4.14). Built once per chip and
# flashed into the `ota_0` partition next to the application by
# tools/pio_recovery.py.
cmake_minimum_required(VERSION 3.16)

# BTP (for the key file's CRC) from the sibling checkout, like test_apps/.
get_filename_component(_tracekit_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED TRACEKIT_BTP_DIR)
    get_filename_component(TRACEKIT_BTP_DIR "${_tracekit_root}/../BTP" ABSOLUTE)
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${TRACEKIT_BTP_DIR}")

# What `btp -recovery_info` and /status report (esp_app_desc_t::version).
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tracekit_recovery)

# The recovery partition is 1 MB in every bundled table. IDF only warns when
# a binary fits some app partition (the factory one does), so check here.
set(_bin "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin")
add_custom_target(recovery_size_check ALL
    COMMAND ${CMAKE_COMMAND} -DBIN=${_bin} -DLIMIT=1048576
            -P ${CMAKE_CURRENT_LIST_DIR}/check_size.cmake
    DEPENDS app
    VERBATIM)
