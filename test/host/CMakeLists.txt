# Host tests: the portable part of TraceKit (no ESP-IDF), built with any
# C++20 compiler.
#
#   cmake -S test/host -B test/host/build
#   cmake --build test/host/build
#   ctest --test-dir test/host/build --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(tracekit_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(TRACEKIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# BTP and TinyShell come from GitHub at the same tags as idf_component.yml.
# Pass -DTRACEKIT_BTP_DIR=<path> / -DTRACEKIT_TINYSHELL_DIR=<path> to use local
# checkouts instead.
include(FetchContent)
set(TRACEKIT_BTP_TAG v2.51.0)
set(TRACEKIT_TINYSHELL_TAG v2.2.0)

# BTP, codec only: no AEAD backend on the host, no BTP test suites.
set(BTP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BTP_ENABLE_AEAD OFF CACHE BOOL "" FORCE)
set(BTP_INSTALL OFF CACHE BOOL "" FORCE)
if(NOT DEFINED TRACEKIT_BTP_DIR)
    FetchContent_Declare(btp
        GIT_REPOSITORY https://github.com/AlisonTristao/BTP.git
        GIT_TAG ${TRACEKIT_BTP_TAG}
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR _download_only)  # add_subdirectory below, EXCLUDE_FROM_ALL
    FetchContent_MakeAvailable(btp)
    set(TRACEKIT_BTP_DIR "${btp_SOURCE_DIR}")
endif()
add_subdirectory("${TRACEKIT_BTP_DIR}" btp EXCLUDE_FROM_ALL)

# TinyShell (ShellLineEditor, for the terminal).
if(NOT DEFINED TRACEKIT_TINYSHELL_DIR)
    FetchContent_Declare(tinyshell
        GIT_REPOSITORY https://github.com/AlisonTristao/TinyShell.git
        GIT_TAG ${TRACEKIT_TINYSHELL_TAG}
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR _download_only)  # add_subdirectory below, EXCLUDE_FROM_ALL
    FetchContent_MakeAvailable(tinyshell)
    set(TRACEKIT_TINYSHELL_DIR "${tinyshell_SOURCE_DIR}")
endif()
add_subdirectory("${TRACEKIT_TINYSHELL_DIR}" tinyshell EXCLUDE_FROM_ALL)
target_compile_definitions(tinyshell PUBLIC TINYSHELL_COLOR)

# Sources that must stay free of ESP-IDF calls. A file lands here once it is
# portable; anything touching drivers stays in the component only.
add_library(tracekit_portable STATIC
    ${TRACEKIT_ROOT}/src/core/ConfigCheck.cpp
    ${TRACEKIT_ROOT}/src/core/DebugReport.cpp
    ${TRACEKIT_ROOT}/src/core/Error.cpp
    ${TRACEKIT_ROOT}/src/core/FrameQueue.cpp
    ${TRACEKIT_ROOT}/src/core/Identity.cpp
    ${TRACEKIT_ROOT}/src/core/RuntimeErrors.cpp
    ${TRACEKIT_ROOT}/src/protocol/ShellText.cpp
    ${TRACEKIT_ROOT}/src/protocol/Terminal.cpp
    ${TRACEKIT_ROOT}/src/protocol/TopicRegistry.cpp
    ${TRACEKIT_ROOT}/src/protocol/SettingsRegistry.cpp
    ${TRACEKIT_ROOT}/src/security/KeyStore.cpp
    ${TRACEKIT_ROOT}/src/security/Sha256.cpp
    ${TRACEKIT_ROOT}/src/update/OtaAuth.cpp
    ${TRACEKIT_ROOT}/src/storage/FileSystem.cpp
    ${TRACEKIT_ROOT}/src/core/StreamFraming.cpp
    ${TRACEKIT_ROOT}/src/core/TimeText.cpp
    ${TRACEKIT_ROOT}/src/transport/WifiLogic.cpp
    ${TRACEKIT_ROOT}/src/transport/WifiNetworks.cpp)
target_include_directories(tracekit_portable PUBLIC
    ${TRACEKIT_ROOT}/include
    ${TRACEKIT_ROOT}/src)
target_link_libraries(tracekit_portable PUBLIC btp::codec btp::node tinyshell)

function(tracekit_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()
tracekit_warnings(tracekit_portable)

enable_testing()

foreach(test_name test_error test_config_check test_frame_queue test_stream_framing test_wifi
                  test_core_pieces test_terminal test_node_presets test_topics
                  test_keystore test_filesystem test_debug_report
                  test_sha256 test_ota_auth test_settings test_time_text)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE tracekit_portable)
    tracekit_warnings(${test_name})
    # MinGW: link the C++ runtime in. A foreign libstdc++-6.dll earlier in
    # PATH (Octave, Anaconda, Qt...) otherwise makes the test die at load
    # with 0xc0000139 behind a Windows error dialog.
    if(MINGW)
        target_link_options(${test_name} PRIVATE -static)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# test_keystore.cpp comes from bally_OS unchanged; its comments quote a shell
# command with trailing `\` continuations.
set_source_files_properties(test_keystore.cpp PROPERTIES COMPILE_OPTIONS "-Wno-comment")
