# 07_sleep_sensor: an ESP32-S3 that wakes, measures, logs, publishes and goes
# back to deep sleep (docs/PLANO.md D31). Nothing wired: the chip's own
# temperature sensor.
cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_LIST_DIR}/../../test_apps/tracekit_components.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tracekit_07_sleep_sensor)
