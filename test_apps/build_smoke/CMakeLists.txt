# Build-only smoke test: proves the component, BTP and TinyShell compile and
# link together for the target. Nothing here runs on hardware.
#
#   pio run -d test_apps/build_smoke                      (PlatformIO)
#   idf.py -C test_apps/build_smoke set-target esp32s3 build   (ESP-IDF)
cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_LIST_DIR}/../btpdev_components.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(build_smoke)
