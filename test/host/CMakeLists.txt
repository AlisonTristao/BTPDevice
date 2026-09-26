# Host tests: the portable part of BTPDevice (no ESP-IDF), built with any
# C++20 compiler.
#
#   cmake -S test/host -B test/host/build
#   cmake --build test/host/build
#   ctest --test-dir test/host/build --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(btpdevice_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(BTPDEV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# BTP from the sibling checkout (same layout as test_apps/), codec only: no
# AEAD backend on the host, no BTP test suites.
if(NOT DEFINED BTPDEV_BTP_DIR)
    get_filename_component(BTPDEV_BTP_DIR "${BTPDEV_ROOT}/../BTP" ABSOLUTE)
endif()
set(BTP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BTP_ENABLE_AEAD OFF CACHE BOOL "" FORCE)
set(BTP_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${BTPDEV_BTP_DIR}" btp EXCLUDE_FROM_ALL)

# Sources that must stay free of ESP-IDF calls. A file lands here once it is
# portable; anything touching drivers stays in the component only.
add_library(btpdev_portable STATIC
    ${BTPDEV_ROOT}/src/core/ConfigCheck.cpp
    ${BTPDEV_ROOT}/src/core/Error.cpp
    ${BTPDEV_ROOT}/src/core/FrameQueue.cpp
    ${BTPDEV_ROOT}/src/core/StreamFraming.cpp
    ${BTPDEV_ROOT}/src/transport/WifiLogic.cpp
    ${BTPDEV_ROOT}/src/transport/WifiNetworks.cpp)
target_include_directories(btpdev_portable PUBLIC
    ${BTPDEV_ROOT}/include
    ${BTPDEV_ROOT}/src)
target_link_libraries(btpdev_portable PUBLIC btp::codec)

function(btpdev_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()
btpdev_warnings(btpdev_portable)

enable_testing()

foreach(test_name test_error test_config_check test_frame_queue test_stream_framing test_wifi)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE btpdev_portable)
    btpdev_warnings(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
